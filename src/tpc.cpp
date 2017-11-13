
#include "XrdHttp/XrdHttpExtHandler.hh"
#include "XrdOuc/XrdOucEnv.hh"
#include "XrdOuc/XrdOucPinPath.hh"
#include "XrdOuc/XrdOucStream.hh"
#include "XrdSec/XrdSecEntity.hh"
#include "XrdSfs/XrdSfsInterface.hh"
#include "XrdVersion.hh"

#include <curl/curl.h>

#include <dlfcn.h>
#include <fcntl.h>

#include <atomic>
#include <memory>
#include <sstream>

#include "XrdTpcVersion.hh"

XrdVERSIONINFO(XrdHttpGetExtHandler, HttpTPC);
extern XrdSfsFileSystem *XrdSfsGetDefaultFileSystem(XrdSfsFileSystem *native_fs,
                                                    XrdSysLogger     *lp,
                                                    const char       *configfn,
                                                    XrdOucEnv        *EnvInfo);


static char *quote(const char *str) {
  int l = strlen(str);
  char *r = (char *) malloc(l*3 + 1);
  r[0] = '\0';
  int i, j = 0;

  for (i = 0; i < l; i++) {
    char c = str[i];

    switch (c) {
      case ' ':
        strcpy(r + j, "%20");
        j += 3;
        break;
      case '[':
        strcpy(r + j, "%5B");
        j += 3;
        break;
      case ']':
        strcpy(r + j, "%5D");
        j += 3;
        break;
      case ':':
        strcpy(r + j, "%3A");
        j += 3;
        break;
      case '/':
        strcpy(r + j, "%2F");
        j += 3;
        break;
      default:
        r[j++] = c;
    }
  }

  r[j] = '\0';

  return r;
}


static XrdSfsFileSystem *load_sfs(void *handle, bool alt, XrdSysError &log, const std::string &libpath, const char *configfn, XrdOucEnv &myEnv, XrdSfsFileSystem *prior_sfs) {
    XrdSfsFileSystem *sfs = nullptr;
    if (alt) {
        auto ep = (XrdSfsFileSystem *(*)(XrdSfsFileSystem *, XrdSysLogger *, const char *, XrdOucEnv *))
                      (dlsym(handle, "XrdSfsGetFileSystem2"));
        if (ep == nullptr) {
            log.Emsg("Config", "Failed to load XrdSfsGetFileSystem2 from library ", libpath.c_str(), dlerror());
            return nullptr;
        }
        sfs = ep(prior_sfs, log.logger(), configfn, &myEnv);
    } else {
        auto ep = (XrdSfsFileSystem *(*)(XrdSfsFileSystem *, XrdSysLogger *, const char *))
                              (dlsym(nullptr, "XrdSfsGetFileSystem"));
        if (ep == nullptr) {
            log.Emsg("Config", "Failed to load XrdSfsGetFileSystem from library ", libpath.c_str(), dlerror());
            return nullptr;
        }
        sfs = ep(prior_sfs, log.logger(), configfn);
    }
    if (!sfs) {
        log.Emsg("Config", "Failed to initialize filesystem library for XrdHttpTPC from ", libpath.c_str());
        return nullptr;
    }
    return sfs;
}

class XrdHttpTPCState {
public:
    XrdHttpTPCState (std::unique_ptr<XrdSfsFile> fh, CURL *curl, bool push) :
        m_push(push),
        m_fh(std::move(fh)),
        m_curl(curl)
    {
        InstallHandlers(curl);
    }

    ~XrdHttpTPCState() {
        if (m_headers) {
            curl_slist_free_all(m_headers);
            m_headers = nullptr;
            curl_easy_setopt(m_curl, CURLOPT_HTTPHEADER, m_headers);
        }
        m_fh->close();
    }

    bool InstallHandlers(CURL *curl) {
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "xrootd-tpc/" XRDTPC_VERSION);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, &XrdHttpTPCState::HeaderCB);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, this);
        if (m_push) {
            curl_easy_setopt(curl, CURLOPT_UPLOAD, 1);
            curl_easy_setopt(curl, CURLOPT_READFUNCTION, &XrdHttpTPCState::ReadCB);
            curl_easy_setopt(curl, CURLOPT_READDATA, this);
            struct stat buf;
            if (SFS_OK == m_fh->stat(&buf)) {
                curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, buf.st_size);
            }
        } else {
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &XrdHttpTPCState::WriteCB);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, this);
        }
        //curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
        return true;
    }

    /**
     * Handle the 'Copy-Headers' feature
     */
    void CopyHeaders(XrdHttpExtReq &req) {
        struct curl_slist *list = NULL;
        for (auto &hdr : req.headers) {
            if (hdr.first == "Copy-Header") {
                list = curl_slist_append(list, hdr.second.c_str());
            }
        }
        if (list != nullptr) {
            curl_easy_setopt(m_curl, CURLOPT_HTTPHEADER, list);
            m_headers = list;
        }
    }

    int GetStatusCode() const {return m_status_code;}

private:
    static size_t HeaderCB(char *buffer, size_t size, size_t nitems, void *userdata) {
        XrdHttpTPCState *obj = static_cast<XrdHttpTPCState*>(userdata);
        std::string header(buffer, size*nitems);
        return obj->Header(header);
    }

    int Header(const std::string &header) {
        // TODO: Handle status codes appropriately.
        printf("Recieved remote header: %s\n", header.c_str());
        if (m_recv_all_headers) {  // This is the second request -- maybe processed a redirect?
            m_recv_all_headers = false;
            m_recv_status_line = false;
        }
        if (!m_recv_status_line) {
            std::stringstream ss(header);
            std::string item;
            if (!std::getline(ss, item, ' ')) return 0;
            m_resp_protocol = item;
            printf("Response protocol: %s\n", m_resp_protocol.c_str());
            if (!std::getline(ss, item, ' ')) return 0;
            try {
                m_status_code = std::stol(item);
            } catch (...) {
                return 0;
            }
            m_recv_status_line = true;
        }
        if (header.size() == 0) {m_recv_all_headers = true;}
        return header.size();
    }

    static size_t WriteCB(void *buffer, size_t size, size_t nitems, void *userdata) {
        XrdHttpTPCState *obj = static_cast<XrdHttpTPCState*>(userdata);
        if (obj->GetStatusCode() < 0) {return 0;}  // malformed request - got body before headers.
        if (obj->GetStatusCode() >= 400) {return 0;}  // Status indicates failure.
        return obj->Write(static_cast<char*>(buffer), size*nitems);
    }

    int Write(char *buffer, size_t size) {
        int retval = m_fh->write(m_offset, buffer, size);
        if (retval == SFS_ERROR) {
            return -1;
        }
        m_offset += retval;
        return retval;
    }

    static size_t ReadCB(void *buffer, size_t size, size_t nitems, void *userdata) {
        XrdHttpTPCState *obj = static_cast<XrdHttpTPCState*>(userdata);
        if (obj->GetStatusCode() < 0) {return 0;}  // malformed request - got body before headers.
        if (obj->GetStatusCode() >= 400) {return 0;}  // Status indicates failure.
        return obj->Read(static_cast<char*>(buffer), size*nitems);
    }

    int Read(char *buffer, size_t size) {
        int retval = m_fh->read(m_offset, buffer, size);
        if (retval == SFS_ERROR) {
            return -1;
        }
        m_offset += retval;
        return retval;
    }

    bool m_push{true};
    bool m_recv_status_line{false};
    bool m_recv_all_headers{false};
    XrdSfsXferSize m_offset{0};
    int m_status_code{-1};
    std::unique_ptr<XrdSfsFile> m_fh;
    CURL *m_curl{nullptr};
    struct curl_slist *m_headers{nullptr};
    std::string m_resp_protocol;
};


class XrdHttpTPC : public XrdHttpExtHandler {
public:
    virtual bool MatchesPath(const char *verb, const char *path) {
        return !strcmp(verb, "COPY") || !strcmp(verb, "OPTIONS");
    }

    virtual int ProcessReq(XrdHttpExtReq &req) {
        if (req.verb == "OPTIONS") {
            return ProcessOptionsReq(req);
        }
        auto header = req.headers.find("Source");
        if (header != req.headers.end()) {
            return ProcessPullReq(header->second, req);
        }
        header = req.headers.find("Destination");
        if (header != req.headers.end()) {
            return ProcessPushReq(header->second, req);
        }
        return req.SendSimpleResp(400, NULL, NULL, (char *)"No Source or Destination specified", 0);
    }

    /**
     * Abstract method in the base class, but does not seem to be used.
     */
    virtual int Init(const char *cfgfile) {
        return 0;
    }

    virtual ~XrdHttpTPC() {
        m_sfs = nullptr;  // NOTE: must delete the SFS here as we may unload the destructor from memory below!
        if (m_handle_base) {
            dlclose(m_handle_base);
            m_handle_base = nullptr;
        }
        if (m_handle_chained) {
            dlclose(m_handle_chained);
            m_handle_chained = nullptr;
        }
    }

    XrdHttpTPC(XrdSysError *log, const char *config, XrdOucEnv *myEnv) :
        m_log(*log)
    {
        if (!Configure(config, myEnv)) {
            throw std::runtime_error("Failed to configure the HTTP third-party-copy handler.");
        }
    }

private:

    /**
     * Handle the OPTIONS verb as we have added a new one...
     */
    int ProcessOptionsReq(XrdHttpExtReq &req) {
      return req.SendSimpleResp(200, NULL, (char *) "DAV: 1\r\nDAV: <http://apache.org/dav/propset/fs/1>\r\nAllow: HEAD,GET,PUT,PROPFIND,DELETE,OPTIONS,COPY", NULL, 0);
    }

    static std::string GetAuthz(XrdHttpExtReq &req) {
        std::string authz;
        auto authz_header = req.headers.find("Authorization");
        if (authz_header != req.headers.end()) {
            char * quoted_url = quote(authz_header->second.c_str());
            std::stringstream ss;
            ss << "authz=" << quoted_url;
            free(quoted_url);
            authz = ss.str();
        }
        return authz;
    }

    int RedirectTransfer(XrdHttpExtReq &req, XrdOucErrInfo &error) {
        int port;
        const char *host = error.getErrText(port);
        if ((host == nullptr) || (*host == '\0') || (port == 0)) {
            char msg[] = "Internal error: redirect without hostname";
            return req.SendSimpleResp(500, nullptr, nullptr, msg, 0);
        }
        std::stringstream ss;
        ss << "Location: http" << (m_desthttps ? "s" : "") << "://" << host << ":" << port << "/" << req.resource;
        return req.SendSimpleResp(307, nullptr, const_cast<char *>(ss.str().c_str()), nullptr, 0);
    }

    int OpenWaitStall(XrdSfsFile &fh, const std::string &resource, int mode, int openMode, const XrdSecEntity &sec,
                      const std::string &authz) {
        int open_result;
        while (1) {
            open_result = fh.open(resource.c_str(), mode, openMode, &sec, authz.empty() ? nullptr : authz.c_str());
            if ((open_result == SFS_STALL) || (open_result == SFS_STARTED)) {
                int secs_to_stall = fh.error.getErrInfo();
                if (open_result == SFS_STARTED) {secs_to_stall = secs_to_stall/2 + 5;}
                sleep(secs_to_stall);
            }
            break;
        }
        return open_result;
    }

    int ProcessPushReq(const std::string & resource, XrdHttpExtReq &req) {
        CURL *curl = curl_easy_init();
        if (!curl) {
            char msg[] = "Failed to initialize internal transfer resources";
            return req.SendSimpleResp(500, nullptr, nullptr, msg, 0);
        }
        char *name = req.GetSecEntity().name;
        std::unique_ptr<XrdSfsFile> fh(m_sfs->newFile(name, m_monid++));
        if (!fh.get()) {
            char msg[] = "Failed to initialize internal transfer file handle";
            return req.SendSimpleResp(500, nullptr, nullptr, msg, 0);
        }
        std::string authz = GetAuthz(req);

        int open_results = OpenWaitStall(*fh, req.resource, SFS_O_RDONLY, 0644, req.GetSecEntity(), authz);
        if (SFS_REDIRECT == open_results) {
            return RedirectTransfer(req, fh->error);
        } else if (SFS_OK != open_results) {
            int code;
            char msg_generic[] = "Failed to open local resource";
            const char *msg = fh->error.getErrText(code);
            if (msg == nullptr) msg = msg_generic;
            int status_code = 400;
            if (code == EACCES) status_code = 401;
            int resp_result = req.SendSimpleResp(status_code, nullptr, nullptr, const_cast<char *>(msg), 0);
            fh->close();
            return resp_result;
        }

        CURLcode res;
        curl_easy_setopt(curl, CURLOPT_URL, resource.c_str());

        XrdHttpTPCState state(std::move(fh), curl, true);
        state.CopyHeaders(req);
        res = curl_easy_perform(curl);
        if (res == CURLE_HTTP_RETURNED_ERROR) {
            m_log.Emsg("ProcessPushReq", "Remote server failed request", curl_easy_strerror(res));
            return req.SendSimpleResp(500, nullptr, nullptr, const_cast<char *>(curl_easy_strerror(res)), 0);
        } else if (state.GetStatusCode() >= 400) {
            std::stringstream ss;
            ss << "Remote side failed with status code " << state.GetStatusCode();
            m_log.Emsg("ProcessPushReq", "Remote server failed request", ss.str().c_str());
            return req.SendSimpleResp(500, nullptr, nullptr, const_cast<char *>(ss.str().c_str()), 0);
        } else if (res) {
            m_log.Emsg("ProcessPushReq", "Curl failed", curl_easy_strerror(res));
            char msg[] = "Unknown internal transfer failure";
            return req.SendSimpleResp(500, nullptr, nullptr, msg, 0);
        } else {
            char msg[] = "Created";
            return req.SendSimpleResp(201, nullptr, nullptr, msg, 0);
        }
    }

    int ProcessPullReq(const std::string &resource, XrdHttpExtReq &req) {
        CURL *curl = curl_easy_init();
        if (!curl) {
            char msg[] = "Failed to initialize internal transfer resources";
            return req.SendSimpleResp(500, nullptr, nullptr, msg, 0);
        }
        char *name = req.GetSecEntity().name;
        std::unique_ptr<XrdSfsFile> fh(m_sfs->newFile(name, m_monid++));
        if (!fh.get()) {
            char msg[] = "Failed to initialize internal transfer file handle";
            return req.SendSimpleResp(500, nullptr, nullptr, msg, 0);
        }
        std::string authz = GetAuthz(req);
        XrdSfsFileOpenMode mode = SFS_O_CREAT;
        auto overwrite_header = req.headers.find("Overwrite");
        if ((overwrite_header == req.headers.end()) || (overwrite_header->second == "T")) {
            mode = SFS_O_TRUNC|SFS_O_POSC;
        }

        int open_result = OpenWaitStall(*fh, req.resource, mode|SFS_O_WRONLY, 0644, req.GetSecEntity(), authz);
        if (SFS_REDIRECT == open_result) {
            return RedirectTransfer(req, fh->error);
        } else if (SFS_OK != open_result) {
            int code;
            char msg_generic[] = "Failed to open local resource";
            const char *msg = fh->error.getErrText(code);
            if ((msg == nullptr) || (*msg == '\0')) msg = msg_generic;
            int status_code = 400;
            if (code == EACCES) status_code = 401;
            if (code == EEXIST) status_code = 412;
            int resp_result = req.SendSimpleResp(status_code, nullptr, nullptr, const_cast<char *>(msg), 0);
            fh->close();
            return resp_result;
        }

        CURLcode res;
        curl_easy_setopt(curl, CURLOPT_URL, resource.c_str());

        XrdHttpTPCState state(std::move(fh), curl, false);
        state.CopyHeaders(req);
        res = curl_easy_perform(curl);
        if (res == CURLE_HTTP_RETURNED_ERROR) {
            m_log.Emsg("ProcessPullReq", "Remote server failed request", curl_easy_strerror(res));
            return req.SendSimpleResp(500, nullptr, nullptr, const_cast<char *>(curl_easy_strerror(res)), 0);
        } else if (state.GetStatusCode() >= 400) {
            std::stringstream ss;
            ss << "Remote side failed with status code " << state.GetStatusCode();
            m_log.Emsg("ProcessPushReq", "Remote server failed request", ss.str().c_str());
            return req.SendSimpleResp(500, nullptr, nullptr, const_cast<char *>(ss.str().c_str()), 0);
        } else if (res) {
            m_log.Emsg("ProcessPullReq", "Curl failed", curl_easy_strerror(res));
            char msg[] = "Unknown internal transfer failure";
            return req.SendSimpleResp(500, nullptr, nullptr, msg, 0);
        } else {
            char msg[] = "Created";
            return req.SendSimpleResp(201, nullptr, nullptr, msg, 0);
        }
    }

    bool ConfigureFSLib(XrdOucStream &Config, std::string &path1, bool &path1_alt, std::string &path2, bool &path2_alt) {
        char *val;
        if (!(val = Config.GetWord())) {
            m_log.Emsg("Config", "fslib not specified");
            return false;
        }
        if (!strcmp("throttle", val)) {
            path2 = "libXrdThrottle.so";
            if (!(val = Config.GetWord())) {
                m_log.Emsg("Config", "fslib throttle target library not specified");
                return false;
            }
        }
        else if (!strcmp("-2", val)) {
            path2_alt = true;
            if (!(val = Config.GetWord())) {
                m_log.Emsg("Config", "fslib library not specified");
                return false;
            }
            path2 = val;
        }
        else {
            path2 = val;
        }
        if (!(val = Config.GetWord()) || !strcmp("default", val)) {
            if (path2 == "libXrdThrottle.so") {
                path1 = "default";
            } else if (!path2.empty()) {
                path1 = path2;
                path2 = "";
                path1_alt = path2_alt;
            }
        } else if (!strcmp("-2", val)) {
            path1_alt = true;
            if (!(val = Config.GetWord())) {
                m_log.Emsg("Config", "fslib base library not specified");
                return false;
            }
            path1 = val;
        } else {
            path2 = val;
        }
        return true;
    }

    bool Configure(const char *configfn, XrdOucEnv *myEnv) {

        XrdOucStream Config(&m_log, getenv("XRDINSTANCE"), myEnv, "=====> ");

        std::string authLib;
        std::string authLibParms;
        int cfgFD = open(configfn, O_RDONLY, 0);
        if (cfgFD < 0) {
            m_log.Emsg("Config", errno, "open config file", configfn);
            return false;
        }
        Config.Attach(cfgFD);
        const char *val;
        std::string path2, path1 = "default";
        bool path1_alt = false, path2_alt = false;
        while ((val = Config.GetMyFirstWord())) {
            if (!strcmp("xrootd.fslib", val)) {
                if (!ConfigureFSLib(Config, path1, path1_alt, path2, path2_alt)) {
                    Config.Close();
                    m_log.Emsg("Config", "Failed to parse the xrootd.fslib directive");
                    return false;
                }
                m_log.Emsg("Config", "xrootd.fslib line successfully processed by XrdHttpTPC");
            } else if (!strcmp("http.desthttps", val)) {
                if (!(val = Config.GetWord())) {
                    Config.Close();
                    m_log.Emsg("Config", "http.desthttps value not specified");
                    return false;
                }
                if (!strcmp("1", val) || !strcasecmp("yes", val) || !strcasecmp("true", val)) {
                    m_desthttps = true;
                } else if (!strcmp("1", val) || !strcasecmp("yes", val) || !strcasecmp("true", val)) {
                    m_desthttps = false;
                } else {
                    Config.Close();
                    m_log.Emsg("Config", "https.dests value is invalid", val);
                    return false;
                }
            }
        }
        Config.Close();

        XrdSfsFileSystem *base_sfs = nullptr;
        if (path1 == "default") {
            m_log.Emsg("Config", "Loading the default filesystem");
            base_sfs = XrdSfsGetDefaultFileSystem(nullptr, m_log.logger(), configfn, myEnv);
            m_log.Emsg("Config", "Finished loading the default filesystem");
        } else {
            char resolvePath[2048];
            bool usedAltPath{true};
            if (!XrdOucPinPath(path1.c_str(), usedAltPath, resolvePath, 2048)) {
                m_log.Emsg("Config", "Failed to locate appropriately versioned base filesystem library for ", path1.c_str());
                return false;
            }
            m_handle_base = dlopen(resolvePath, RTLD_LOCAL|RTLD_NOW);
            if (m_handle_base == nullptr) {
                m_log.Emsg("Config", "Failed to base plugin ", resolvePath, dlerror());
                return false;
            }
            base_sfs = load_sfs(m_handle_base, path1_alt, m_log, path1, configfn, *myEnv, nullptr);
        }
        if (!base_sfs) {
            m_log.Emsg("Config", "Failed to initialize filesystem library for XrdHttpTPC from ", path1.c_str());
            return false;
        }
        XrdSfsFileSystem *chained_sfs = nullptr;
        if (!path2.empty()) {
            char resolvePath[2048];
            bool usedAltPath{true};
            if (!XrdOucPinPath(path2.c_str(), usedAltPath, resolvePath, 2048)) {
                m_log.Emsg("Config", "Failed to locate appropriately versioned chained filesystem library for ", path2.c_str());
                return false;
            }
            m_handle_chained = dlopen(resolvePath, RTLD_LOCAL|RTLD_NOW);
            if (m_handle_chained == nullptr) {
                m_log.Emsg("Config", "Failed to chained plugin ", resolvePath, dlerror());
                return false;
            }
            chained_sfs = load_sfs(m_handle_chained, path2_alt, m_log, path2, configfn, *myEnv, base_sfs);
        }
        m_sfs.reset(chained_sfs ? chained_sfs : base_sfs);
        m_log.Emsg("Config", "Successfully configured the filesystem object for XrdHttpTPC");
        return true;
    }

    bool m_desthttps{false};
    static std::atomic<uint64_t> m_monid;
    XrdSysError &m_log;
    std::unique_ptr<XrdSfsFileSystem> m_sfs;
    void *m_handle_base{nullptr};
    void *m_handle_chained{nullptr};
};

std::atomic<uint64_t> XrdHttpTPC::m_monid{0};

extern "C" {

XrdHttpExtHandler *XrdHttpGetExtHandler(XrdSysError *log, const char * config, const char * /*parms*/, XrdOucEnv *myEnv) {
    if (curl_global_init(CURL_GLOBAL_DEFAULT)) {
        log->Emsg("Initialize", "libcurl failed to initialize");
        return nullptr;
    }

    XrdHttpTPC *retval{nullptr};
    if (!config) {
        log->Emsg("Initialize", "XrdHttpTPC requires a config filename in order to load");
        return nullptr;
    }
    try {
        log->Emsg("Initialize", "Will load configuration for XrdHttpTPC from", config);
        retval = new XrdHttpTPC(log, config, myEnv);
    } catch (std::runtime_error &re) {
        log->Emsg("Initialize", "Encountered a runtime failure when loading ", re.what());
        printf("Provided env vars: %p, XrdInet*: %p\n", myEnv, myEnv->GetPtr("XrdInet*"));
    }
    return retval;
}

}
