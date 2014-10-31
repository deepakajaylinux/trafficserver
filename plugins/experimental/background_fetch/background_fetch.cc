/** @file

    Plugin to perform background fetches of certain content that would
    otherwise not be cached. For example, Range: requests / responses.

    @section license License

    Licensed to the Apache Software Foundation (ASF) under one
    or more contributor license agreements.  See the NOTICE file
    distributed with this work for additional information
    regarding copyright ownership.  The ASF licenses this file
    to you under the Apache License, Version 2.0 (the
    "License"); you may not use this file except in compliance
    with the License.  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

#include <stdio.h>
#include <string.h>
#include <string>
#include <stdarg.h>
#include <getopt.h>

#include <netinet/in.h>
#include <arpa/inet.h>

#include "ts/ts.h"
#include "ts/remap.h"
#include "ink_defs.h"
#include <set>


// Some wonkiness around compiler version and the unordered map (hash)
#if HAVE_UNORDERED_MAP
#  include <unordered_map>
   typedef std::unordered_map<std::string, bool> OutstandingRequests;
#else
#  include <map>
   typedef std::map<std::string, bool> OutstandingRequests;
#endif

// Constants
const char PLUGIN_NAME[] = "background_fetch";
static int g_background_fetch_ArgIndex = 0;

// Types
typedef std::set<std::string> stringSet;

typedef struct {
  stringSet contentTypeSet;
  stringSet userAgentSet;
  stringSet clientIpSet;
} ExclusionSet;

// Global config, if we don't have a remap specific config.
static ExclusionSet gExclusionSet;


///////////////////////////////////////////////////////////////////////////
// Read a config file
//
static bool
read_config(char* config_file, ExclusionSet* ri)
{
  char file_path[1024];
  TSFile file;

  if (config_file == NULL) {
    TSError("%s: invalid config file", PLUGIN_NAME);
    return false;
  }

  TSDebug(PLUGIN_NAME, "trying to open config file in this path: %s", config_file);

  file = TSfopen(config_file, "r");
  if (file == NULL) {
    TSDebug(PLUGIN_NAME, "Failed to open config file %s, trying rel path", config_file);
    snprintf(file_path, sizeof(file_path), "%s/%s", TSInstallDirGet(), config_file);
    file = TSfopen(file_path, "r");
    if (file == NULL) {
      TSError("%s: invalid config file", PLUGIN_NAME);
      return false;
    }
  }

  stringSet* contentTypeSetP = &(ri->contentTypeSet);
  stringSet* userAgentSetP = &(ri->userAgentSet);
  stringSet* clientIpSetP = &(ri->clientIpSet);
  char buffer[8192];

  memset(buffer, 0, sizeof(buffer));
  while (TSfgets(file, buffer, sizeof(buffer) - 1) != NULL) {
    char *eol = 0;
    // make sure line was not bigger than buffer
    if ((eol = strchr(buffer, '\n')) == NULL && (eol = strstr(buffer, "\r\n")) == NULL) {
      TSError("%s: exclusion line too long, did not get a good line in cfg, skipping, line: %s", PLUGIN_NAME, buffer);
      memset(buffer, 0, sizeof(buffer));
      continue;
    }
    // make sure line has something useful on it
    if (eol - buffer < 2 || buffer[0] == '#') {
      memset(buffer, 0, sizeof(buffer));
      continue;
    }

    char *savePtr = NULL;
    char* cfg = strtok_r(buffer, "\n\r\n", &savePtr);

    if (cfg != NULL) {
      char* cfg_type = strtok_r(buffer, " ", &savePtr);
      char* cfg_value = NULL;

      TSDebug(PLUGIN_NAME, "setting background_fetch exclusion criterion based on string: %s", cfg);

      if (cfg_type) {
        cfg_value = strtok_r(NULL, " ", &savePtr);
      }

      if (cfg_type && cfg_value) {
        if (!strcmp(cfg_type, "Content-Type")) {
          TSDebug(PLUGIN_NAME, "adding content-type %s", cfg_value);
          contentTypeSetP->insert(cfg_value);
        } else if (!strcmp(cfg_type, "User-Agent")) {
          TSDebug(PLUGIN_NAME, "adding user-agent %s", cfg_value);
          userAgentSetP->insert(cfg_value);
        } else if (!strcmp(cfg_type, "Client-IP")) {
          TSDebug(PLUGIN_NAME, "adding client-ip %s", cfg_value);
          clientIpSetP->insert(cfg_value);
        } else {
          TSError("%s: Unknown config type: %s", PLUGIN_NAME, cfg_type);
        }
      }
      memset(buffer, 0, sizeof(buffer));
    }
  }
  TSfclose(file);

  TSDebug(PLUGIN_NAME, "Done parsing config");

  return true;
}


///////////////////////////////////////////////////////////////////////////
// Remove a header (fully) from an TSMLoc / TSMBuffer. Return the number
// of fields (header values) we removed.
int
remove_header(TSMBuffer bufp, TSMLoc hdr_loc, const char* header, int len)
{
  TSMLoc field = TSMimeHdrFieldFind(bufp, hdr_loc, header, len);
  int cnt = 0;

  while (field) {
    TSMLoc tmp = TSMimeHdrFieldNextDup(bufp, hdr_loc, field);

    ++cnt;
    TSMimeHdrFieldDestroy(bufp, hdr_loc, field);
    TSHandleMLocRelease(bufp, hdr_loc, field);
    field = tmp;
  }

  return cnt;
}


///////////////////////////////////////////////////////////////////////////
// Set a header to a specific value. This will avoid going to through a
// remove / add sequence in case of an existing header.
// but clean.
bool
set_header(TSMBuffer bufp, TSMLoc hdr_loc, const char* header, int len, const char* val, int val_len)
{
  if (!bufp || !hdr_loc || !header || len <= 0 || !val || val_len <= 0) {
    return false;
  }

  bool ret = false;
  TSMLoc field_loc = TSMimeHdrFieldFind(bufp, hdr_loc, header, len);

  if (!field_loc) {
    // No existing header, so create one
    if (TS_SUCCESS == TSMimeHdrFieldCreateNamed(bufp, hdr_loc, header, len, &field_loc)) {
      if (TS_SUCCESS == TSMimeHdrFieldValueStringSet(bufp, hdr_loc, field_loc, -1, val, val_len)) {
        TSMimeHdrFieldAppend(bufp, hdr_loc, field_loc);
        ret = true;
      }
      TSHandleMLocRelease(bufp, hdr_loc, field_loc);
    }
  } else {
    TSMLoc tmp = NULL;
    bool first = true;

    while (field_loc) {
      if (first) {
        first = false;
        if (TS_SUCCESS == TSMimeHdrFieldValueStringSet(bufp, hdr_loc, field_loc, -1, val, val_len)) {
          ret = true;
        }
      } else {
        TSMimeHdrFieldDestroy(bufp, hdr_loc, field_loc);
      }
      tmp = TSMimeHdrFieldNextDup(bufp, hdr_loc, field_loc);
      TSHandleMLocRelease(bufp, hdr_loc, field_loc);
      field_loc = tmp;
    }
  }

  return ret;
}


///////////////////////////////////////////////////////////////////////////
// Dump a header on stderr, useful together with TSDebug().
void
dump_headers(TSMBuffer bufp, TSMLoc hdr_loc)
{
  TSIOBuffer output_buffer;
  TSIOBufferReader reader;
  TSIOBufferBlock block;
  const char* block_start;
  int64_t block_avail;

  output_buffer = TSIOBufferCreate();
  reader = TSIOBufferReaderAlloc(output_buffer);

  /* This will print  just MIMEFields and not the http request line */
  TSMimeHdrPrint(bufp, hdr_loc, output_buffer);

  /* We need to loop over all the buffer blocks, there can be more than 1 */
  block = TSIOBufferReaderStart(reader);
  do {
    block_start = TSIOBufferBlockReadStart(block, reader, &block_avail);
    if (block_avail > 0) {
      TSDebug(PLUGIN_NAME, "Headers are:\n%.*s", static_cast<int>(block_avail), block_start);
    }
    TSIOBufferReaderConsume(reader, block_avail);
    block = TSIOBufferReaderStart(reader);
  } while (block && block_avail != 0);

  /* Free up the TSIOBuffer that we used to print out the header */
  TSIOBufferReaderFree(reader);
  TSIOBufferDestroy(output_buffer);
}


///////////////////////////////////////////////////////////////////////////
// Struct to hold configurations and state. This can be global, or per
// remap rule. This also holds the list of currently outstanding URLs,
// such that we can avoid sending more than one background fill per URL at
// any given time.
class BGFetchConfig
{
public:
  BGFetchConfig()
    : log(NULL)
  {
    _lock = TSMutexCreate();
  }

  ~BGFetchConfig()
  {
    // ToDo: Destroy mutex ? TS-1432
  }

  void create_log(const char* log_name)
  {
    TSDebug(PLUGIN_NAME, "Creating log name %s\n", log_name);
    TSAssert(TS_SUCCESS == TSTextLogObjectCreate(log_name, TS_LOG_MODE_ADD_TIMESTAMP, &log));
  }

  bool acquire(const std::string &url)
  {
    bool ret;

    TSMutexLock(_lock);
    if (_urls.end() == _urls.find(url)) {
      _urls[url] = true;
      ret = true;
    } else {
      ret = false;
    }
    TSMutexUnlock(_lock);

    return ret;
  }

  bool release(const std::string &url)
  {
    bool ret;

    TSMutexLock(_lock);
    if (_urls.end() == _urls.find(url)) {
      ret = false;
    } else {
      _urls.erase(url);
      ret = true;
    }
    TSMutexUnlock(_lock);

    return ret;
  }

  TSTextLogObject log;

private:
  OutstandingRequests _urls;
  TSMutex _lock;
};

BGFetchConfig* gConfig;


//////////////////////////////////////////////////////////////////////////////
// Hold and manage some state for the background fetch continuation
// This is necessary, because the TXN is likely to not be available
// during the time we fetch from origin.
static int cont_bg_fetch(TSCont contp, TSEvent event, void* edata);

struct BGFetchData
{
  BGFetchData(BGFetchConfig* cfg=gConfig)
    : hdr_loc(TS_NULL_MLOC), url_loc(TS_NULL_MLOC), vc(NULL), _bytes(0), _cont(NULL),  _config(cfg)
  {
    mbuf = TSMBufferCreate();
  }

  ~BGFetchData()
  {
    release_url();

    TSHandleMLocRelease(mbuf, TS_NULL_MLOC, hdr_loc);
    TSHandleMLocRelease(mbuf, TS_NULL_MLOC, url_loc);

    TSMBufferDestroy(mbuf);

    if (vc) {
      TSError("%s: Destroyed BGFetchDATA while VC was alive", PLUGIN_NAME);
      TSVConnClose(vc);
      vc = NULL;
    }

    // If we got schedule, also clean that up
    if (_cont) {
      TSContDestroy(_cont);

      TSIOBufferReaderFree(req_io_buf_reader);
      TSIOBufferDestroy(req_io_buf);
      TSIOBufferReaderFree(resp_io_buf_reader);
      TSIOBufferDestroy(resp_io_buf);
    }
  }

  bool acquire_url() const { return _config->acquire(_url); }
  bool release_url() const { return _config->release(_url); }

  const char* get_url() const { return _url.c_str(); }
  void add_bytes(int64_t b) { _bytes += b; }

  bool initialize(TSMBuffer request, TSMLoc req_hdr, TSHttpTxn txnp);
  void schedule();
  void log(TSEvent event) const;

  TSMBuffer mbuf;
  TSMLoc hdr_loc;
  TSMLoc url_loc;
  struct sockaddr_storage client_ip;

  // This is for the actual background fetch / NetVC
  TSVConn vc;
  TSIOBuffer req_io_buf, resp_io_buf;
  TSIOBufferReader req_io_buf_reader, resp_io_buf_reader;
  TSVIO r_vio, w_vio;

private:
  std::string _url;
  int64_t _bytes;
  TSCont _cont;
  BGFetchConfig* _config;
};


// This sets up the data and continuation properly, this is done outside
// of the CTor, since this can actually fail. If we fail, the data is
// useless, and should be delete'd.
//
// This needs the txnp temporarily, so it can copy the pristine request
// URL. The txnp is not used once initialize() returns.
//
// Upon succesful completion, the struct should be ready to start a
// background fetch.
bool
BGFetchData::initialize(TSMBuffer request, TSMLoc req_hdr, TSHttpTxn txnp)
{
  TSAssert(TS_NULL_MLOC == hdr_loc);
  TSAssert(TS_NULL_MLOC == url_loc);
  struct sockaddr const* ip = TSHttpTxnClientAddrGet(txnp);

  if (ip) {
    if (ip->sa_family == AF_INET) {
      memcpy(&client_ip, ip, sizeof(sockaddr_in));
    } else if (ip->sa_family == AF_INET6) {
      memcpy(&client_ip, ip, sizeof(sockaddr_in6));
    } else {
      TSError("%s: Unknown address family %d", PLUGIN_NAME, ip->sa_family);
    }
  } else {
    TSError("%s: failed to get client host info", PLUGIN_NAME);
    return false;
  }

  hdr_loc = TSHttpHdrCreate(mbuf);
  if (TS_SUCCESS == TSHttpHdrCopy(mbuf, hdr_loc, request, req_hdr)) {
    TSMLoc purl;
    int len;

    // Now copy the pristine request URL into our MBuf
    if ((TS_SUCCESS == TSHttpTxnPristineUrlGet(txnp, &request, &purl)) &&
        (TS_SUCCESS == TSUrlClone(mbuf, request, purl, &url_loc))) {
      char* url = TSUrlStringGet(mbuf, url_loc, &len);

      _url.append(url, len); // Save away the URL for later use when acquiring lock
      TSfree(static_cast<void*>(url));

      if (TS_SUCCESS == TSHttpHdrUrlSet(mbuf, hdr_loc, url_loc)) {
        // Make sure we have the correct Host: header for this request.
        const char *hostp = TSUrlHostGet(mbuf, url_loc, &len);

        if (set_header(mbuf, hdr_loc, TS_MIME_FIELD_HOST, TS_MIME_LEN_HOST, hostp, len)) {
          TSDebug(PLUGIN_NAME, "Set header Host: %.*s", len, hostp);
        }

        // Next, remove any Range: headers from our request.
        if (remove_header(mbuf, hdr_loc, TS_MIME_FIELD_RANGE, TS_MIME_LEN_RANGE) > 0) {
          TSDebug(PLUGIN_NAME, "Removed the Range: header from request");
        }

        return true;
      }
    }
  }

  // Something failed.
  return false;
}


// Create, setup and schedule the background fetch continuation.
void
BGFetchData::schedule()
{
  TSAssert(NULL == _cont);

  // Setup the continuation
  _cont = TSContCreate(cont_bg_fetch, TSMutexCreate());
  TSContDataSet(_cont, static_cast<void*>(this));

  // Initialize the VIO stuff (for the fetch)
  req_io_buf = TSIOBufferCreate();
  req_io_buf_reader = TSIOBufferReaderAlloc(req_io_buf);
  resp_io_buf = TSIOBufferCreate();
  resp_io_buf_reader = TSIOBufferReaderAlloc(resp_io_buf);

  // Schedule
  TSContSchedule(_cont, 0, TS_THREAD_POOL_NET);
}


// Log format is:
//    remap-tag bytes status url
void
BGFetchData::log(TSEvent event) const
{
  BGFetchConfig* conf = _config ? _config : gConfig;

  if (conf->log) {
    const char* status;

    switch (event) {
    case TS_EVENT_VCONN_EOS:
      status = "EOS";
      break;
    case TS_EVENT_VCONN_INACTIVITY_TIMEOUT:
      status = "TIMEOUT";
      break;
    case TS_EVENT_ERROR:
      status = "ERROR";
      break;
    case TS_EVENT_VCONN_READ_COMPLETE:
      status = "READ_COMP";
      break;
    default:
      status = "UNKNOWN";
      break;
    }

    // ToDo: Also deal with per-remap tagging
    TSTextLogObjectWrite(conf->log, "%s %" PRId64 " %s %s", "-", _bytes, status, _url.c_str());
  }
}


//////////////////////////////////////////////////////////////////////////////
// Continuation to perform a background fill of a URL. This is pretty
// expensive (memory allocations etc.), we could eliminate maybe the
// std::string, but I think it's fine for now.
static int
cont_bg_fetch(TSCont contp, TSEvent event, void* /* edata ATS_UNUSED */)
{
  BGFetchData* data = static_cast<BGFetchData*>(TSContDataGet(contp));
  int64_t avail;

  switch (event) {
  case TS_EVENT_IMMEDIATE:
  case TS_EVENT_TIMEOUT:
    // Debug info for this particular bg fetch (put all debug in here please)
    if (TSIsDebugTagSet(PLUGIN_NAME)) {
      char buf[INET6_ADDRSTRLEN];
      const sockaddr* sockaddress = (const sockaddr*)&data->client_ip;

      switch (sockaddress->sa_family) {
      case AF_INET:
        inet_ntop(AF_INET, &(((struct sockaddr_in *) sockaddress)->sin_addr), buf, INET_ADDRSTRLEN);
        TSDebug(PLUGIN_NAME, "Client IPv4 = %s", buf);
        break;
      case AF_INET6:
        inet_ntop(AF_INET6, &(((struct sockaddr_in6 *) sockaddress)->sin6_addr), buf, INET6_ADDRSTRLEN);
        TSDebug(PLUGIN_NAME, "Client IPv6 = %s", buf);
        break;
      default:
        TSError("%s: Unknown address family %d", PLUGIN_NAME, sockaddress->sa_family);
        break;
      }
      TSDebug(PLUGIN_NAME, "Starting bg fetch on: %s", data->get_url());
      dump_headers(data->mbuf, data->hdr_loc);
    }

    // Setup the NetVC for background fetch
    TSAssert(NULL == data->vc);
    if ((data->vc = TSHttpConnect((sockaddr*)&data->client_ip)) != NULL) {
      TSHttpHdrPrint(data->mbuf, data->hdr_loc, data->req_io_buf);
      // We never send a body with the request. ToDo: Do we ever need to support that ?
      TSIOBufferWrite(data->req_io_buf, "\r\n", 2);

      data->r_vio = TSVConnRead(data->vc, contp, data->resp_io_buf, INT64_MAX);
      data->w_vio = TSVConnWrite(data->vc, contp, data->req_io_buf_reader,
                                 TSIOBufferReaderAvail(data->req_io_buf_reader));
    } else {
      delete data;
      TSError("%s: failed to connect to internal process, major malfunction", PLUGIN_NAME);
    }

  case TS_EVENT_VCONN_WRITE_COMPLETE:
    TSDebug(PLUGIN_NAME, "Write Complete");
    break;

  case TS_EVENT_VCONN_READ_READY:
    avail = TSIOBufferReaderAvail(data->resp_io_buf_reader);
    data->add_bytes(avail);
    TSIOBufferReaderConsume(data->resp_io_buf_reader, avail);
    TSVIONDoneSet(data->r_vio, TSVIONDoneGet(data->r_vio) + avail);
    TSVIOReenable(data->r_vio);
    break;

  case TS_EVENT_VCONN_READ_COMPLETE:
  case TS_EVENT_VCONN_EOS:
  case TS_EVENT_VCONN_INACTIVITY_TIMEOUT:
  case TS_EVENT_ERROR:
    if (event == TS_EVENT_VCONN_INACTIVITY_TIMEOUT) {
      TSDebug(PLUGIN_NAME, "Encountered Inactivity Timeout");
      TSVConnAbort(data->vc, TS_VC_CLOSE_ABORT);
    } else {
      TSVConnClose(data->vc);
    }

    TSDebug(PLUGIN_NAME, "Closing down background transaction, event= %s(%d)", TSHttpEventNameLookup(event), event);
    avail = TSIOBufferReaderAvail(data->resp_io_buf_reader);
    data->add_bytes(avail);
    TSIOBufferReaderConsume(data->resp_io_buf_reader, avail);
    TSVIONDoneSet(data->r_vio, TSVIONDoneGet(data->r_vio) + avail);

    data->log(event);
    data->vc = NULL;
    // Close, release and cleanup
    delete data;
    break;

  default:
    TSDebug(PLUGIN_NAME, "Unhandled event: %s (%d)", TSHttpEventNameLookup(event), event);
    break;
  }

  return 0;
}

///////////////////////////////////////////////////////////////////////////
// This is a TXN hook, used to verify that the response (before sending to
// originating client) is indeed cacheable. This has to be deferred, because
// we might have plugins that changes the cacheability of an Origin response.
//
static int
cont_check_cacheable(TSCont contp, TSEvent /* event ATS_UNUSED */, void* edata)
{
  // ToDo: If we want to support per-remap configurations, we have to pass along the data here
  TSHttpTxn txnp = static_cast<TSHttpTxn>(edata);
  TSMBuffer response, request;
  TSMLoc resp_hdr, req_hdr;

  if (TS_SUCCESS == TSHttpTxnServerRespGet(txnp, &response, &resp_hdr)) {
    if (TS_SUCCESS == TSHttpTxnClientReqGet(txnp, &request, &req_hdr)) {
      // Temporarily change the response status to 200 OK, so we can reevaluate cacheability.
      TSHttpHdrStatusSet(response, resp_hdr, TS_HTTP_STATUS_OK);
      bool cacheable = TSHttpTxnIsCacheable(txnp, NULL, response);
      TSHttpHdrStatusSet(response, resp_hdr, TS_HTTP_STATUS_PARTIAL_CONTENT);

      TSDebug(PLUGIN_NAME, "Testing: request / response is cacheable?");
      if (cacheable) {
        BGFetchData* data = new BGFetchData();

        // Initialize the data structure (can fail) and acquire a privileged lock on the URL
        if (data->initialize(request, req_hdr, txnp) && data->acquire_url()) {
          data->schedule();
        } else {
          delete data; // Not sure why this would happen, but ok.
        }
      }
      // Release the request MLoc
      TSHandleMLocRelease(request, TS_NULL_MLOC, req_hdr);
    }
    // Release the response MLoc
    TSHandleMLocRelease(response, TS_NULL_MLOC, resp_hdr);
  }

  // Reenable and continue with the state machine.
  TSContDestroy(contp);
  TSHttpTxnReenable(txnp, TS_EVENT_HTTP_CONTINUE);

  return 0;
}


///////////////////////////////////////////////////////////////////////////
// Check if a header excludes us from running the background fetch
//
static bool
check_hdr_configured(TSMBuffer hdr_bufp, TSMLoc req_hdrs, const char* field_type, int field_len, stringSet* cfg_set)
{
  bool hdr_found = false;

  TSMLoc loc = TSMimeHdrFieldFind(hdr_bufp, req_hdrs, field_type, field_len);

  if (TS_NULL_MLOC != loc) {
    int val_len = 0;
    const char *val_str = TSMimeHdrFieldValueStringGet(hdr_bufp, req_hdrs, loc, 0, &val_len);

    if (!val_str || val_len <= 0) {
      TSDebug(PLUGIN_NAME,"invalid content type");
    } else {
      stringSet::iterator it = cfg_set->begin();

      while(it!=cfg_set->end()) {
        TSDebug(PLUGIN_NAME, "comparing with %s", (*it).c_str());
        if (NULL != strstr(val_str, (*it).c_str())) {
          TSDebug(PLUGIN_NAME,"excluding bg fetch for configured field %s", (*it).c_str());
          hdr_found = true;
          break;
        }
        it++;
      }
      TSHandleMLocRelease(hdr_bufp, req_hdrs, loc);
    }
  } else {
    TSDebug(PLUGIN_NAME, "no field %s in request header", field_type);
  }
  return hdr_found;
}


///////////////////////////////////////////////////////////////////////////
// Check the configuration (either per remap, or global), and decide if
// this request is allowed to trigger a background fetch.
//
static bool
is_background_fetch_allowed(TSHttpTxn txnp, ExclusionSet* ri)
{
  TSDebug(PLUGIN_NAME, "Testing: request is internal?");
  if (TSHttpIsInternalRequest(txnp) == TS_SUCCESS) {
    return false;
  }

  bool allow_bg_fetch = true;

  stringSet* contentTypeSetP = &(ri->contentTypeSet);
  stringSet* userAgentSetP = &(ri->userAgentSet);
  stringSet* clientIpSetP = &(ri->clientIpSet);

  const sockaddr* client_ip = TSHttpTxnClientAddrGet(txnp);
  char ip_buf[INET6_ADDRSTRLEN];

  if(AF_INET == client_ip->sa_family) {
    inet_ntop(AF_INET, &(reinterpret_cast<const sockaddr_in*>(client_ip)->sin_addr), ip_buf, INET_ADDRSTRLEN);
  } else if(AF_INET6 == client_ip->sa_family) {
    inet_ntop(AF_INET6, &(reinterpret_cast<const sockaddr_in6*>(client_ip)->sin6_addr), ip_buf, INET6_ADDRSTRLEN);
  } else {
    TSError("%s: unknown family %d", PLUGIN_NAME, client_ip->sa_family);
  }

  TSDebug(PLUGIN_NAME,"client_ip %s", ip_buf);

  for (stringSet::iterator it=clientIpSetP->begin(); it != clientIpSetP->end(); ++it) {
    const char* cfg_ip = (*it).c_str();

    if ((strlen(cfg_ip) == strlen(ip_buf)) && !strcmp(cfg_ip, ip_buf)) {
      TSDebug(PLUGIN_NAME,"excluding bg fetch for ip %s, configured ip %s", ip_buf, cfg_ip);
      allow_bg_fetch = false;
      break;
    }
  }

  if (!allow_bg_fetch) {
    return false;
  }

  TSMBuffer hdr_bufp;
  TSMLoc req_hdrs;

  if (TSHttpTxnClientReqGet(txnp, &hdr_bufp, &req_hdrs) == TS_SUCCESS) {
    if (check_hdr_configured (hdr_bufp, req_hdrs, TS_MIME_FIELD_CONTENT_TYPE, TS_MIME_LEN_CONTENT_TYPE, contentTypeSetP)) {
      TSDebug(PLUGIN_NAME, "found content-type match");
      allow_bg_fetch = false;
      goto done;
    }
    if (check_hdr_configured (hdr_bufp, req_hdrs, TS_MIME_FIELD_USER_AGENT, TS_MIME_LEN_USER_AGENT, userAgentSetP)) {
      TSDebug(PLUGIN_NAME, "found user-agent match");
      allow_bg_fetch = false;
      goto done;
    }
  } else {
    // something wrong..
    TSError ("%s: Failed to get req headers", PLUGIN_NAME);
    return false;
  }

  done:
  TSHandleMLocRelease(hdr_bufp, TS_NULL_MLOC, req_hdrs);
  return allow_bg_fetch;
}


//////////////////////////////////////////////////////////////////////////////
// Main "plugin", which is a global READ_RESPONSE_HDR hook. Before
// initiating a background fetch, this checks:
//
//     1. Check if a background fetch is allowed for this request
// and
//     2. Is the response from origin a 206 (Partial)?
//
// We defer the check on cacheability to the SEND_RESPONSE_HDR hook, since
// there could be other plugins that modifies the response after us.
//
static int
cont_handle_response(TSCont /* contp ATS_UNUSED */, TSEvent /* event ATS_UNUSED */, void* edata)
{
  // ToDo: If we want to support per-remap configurations, we have to pass along the data here
  TSHttpTxn txnp = static_cast<TSHttpTxn>(edata);
  ExclusionSet *ri = static_cast<ExclusionSet *> (TSHttpTxnArgGet(txnp, g_background_fetch_ArgIndex));

  if (ri == NULL) {
    ri = &gExclusionSet;
  }

  if (is_background_fetch_allowed(txnp, ri)) {
    TSMBuffer response;
    TSMLoc resp_hdr;

    if (TS_SUCCESS == TSHttpTxnServerRespGet(txnp, &response, &resp_hdr)) {
      // ToDo: Check the MIME type first, to see if it's a type we care about.
      // ToDo: Such MIME types should probably be per remap rule.

      // 2. Only deal with 206 responses from Origin
      TSDebug(PLUGIN_NAME, "Testing: response is 206?");
      if (TS_HTTP_STATUS_PARTIAL_CONTENT == TSHttpHdrStatusGet(response, resp_hdr)) {
        // Everything looks good so far, add a TXN hook for SEND_RESPONSE_HDR
        TSCont contp = TSContCreate(cont_check_cacheable, NULL);

        TSHttpTxnHookAdd(txnp, TS_HTTP_SEND_RESPONSE_HDR_HOOK, contp);
      }
      // Release the response MLoc
      TSHandleMLocRelease(response, TS_NULL_MLOC, resp_hdr);
    }
  }

  // Reenable and continue with the state machine.
  TSHttpTxnReenable(txnp, TS_EVENT_HTTP_CONTINUE);
  return 0;
}


///////////////////////////////////////////////////////////////////////////
// Setup global hooks
void
TSPluginInit(int argc, const char* argv[])
{
  TSPluginRegistrationInfo info;
  static const struct option longopt[] = {
    { const_cast<char *>("log"), required_argument, NULL, 'l' },
    { const_cast<char *>("exclude"), required_argument, NULL, 'e' },
    { NULL, no_argument, NULL, '\0' }
  };

  info.plugin_name = (char*)PLUGIN_NAME;
  info.vendor_name = (char*)"Apache Software Foundation";
  info.support_email = (char*)"dev@trafficserver.apache.org";

  if (TS_SUCCESS != TSPluginRegister(TS_SDK_VERSION_3_0 , &info)) {
    TSError("%s: plugin registration failed.\n", PLUGIN_NAME);
  }

  if (TSHttpArgIndexReserve("background_fetch", "exclustion criteria index", &g_background_fetch_ArgIndex) != TS_SUCCESS) {
    TSError("%s: Failed to reserve an argument index", PLUGIN_NAME);
    TSAssert(0);
    return;
  }

  gConfig = new BGFetchConfig();
  optind = 1;

  while (true) {
    int opt = getopt_long(argc, (char* const *)argv, "le", longopt, NULL);

    switch (opt) {
    case 'l':
      gConfig->create_log(optarg);
      break;
    case 'e':
      TSDebug(PLUGIN_NAME, "config file %s..", optarg);
      read_config(optarg, &gExclusionSet);
      break;
    }

    if (opt == -1) {
      break;
    }
  }

  TSDebug(PLUGIN_NAME, "Initialized");
  TSHttpHookAdd(TS_HTTP_READ_RESPONSE_HDR_HOOK, TSContCreate(cont_handle_response, NULL));
}


///////////////////////////////////////////////////////////////////////////
// Setup Remap mode
///////////////////////////////////////////////////////////////////////////////
// Initialize the plugin.
//
TSReturnCode
TSRemapInit(TSRemapInterface *api_info, char *errbuf, int errbuf_size)
{
  TSDebug(PLUGIN_NAME, "background fetch remap init");
  if (!api_info) {
    strncpy(errbuf, "[tsremap_init] - Invalid TSRemapInterface argument", errbuf_size - 1);
    return TS_ERROR;
  }

  if (api_info->tsremap_version < TSREMAP_VERSION) {
    snprintf(errbuf, errbuf_size - 1, "[TSRemapInit] - Incorrect API version %ld.%ld",
             api_info->tsremap_version >> 16, (api_info->tsremap_version & 0xffff));
    return TS_ERROR;
  }

  TSDebug(PLUGIN_NAME, "background fetch remap is successfully initialized");
  return TS_SUCCESS;
}


///////////////////////////////////////////////////////////////////////////////
// We don't have any specific "instances" here, at least not yet.
//
TSReturnCode
TSRemapNewInstance(int  argc, char* argv[], void** ih, char* /* errbuf */, int /* errbuf_size */)
{
  ExclusionSet *ri = new ExclusionSet();
  if (ri == NULL) {
    TSError("%s:Unable to create remap instance", PLUGIN_NAME);
    return TS_ERROR;
  }

  char* fileName = NULL;
  if (argc > 2) {
    fileName = argv[2];
    TSDebug(PLUGIN_NAME, "config file %s", fileName);
  }

  read_config(fileName, ri);

  *ih = (void*)ri;

  return TS_SUCCESS;
}

void
TSRemapDeleteInstance(void* ih)
{
  ExclusionSet* ri = static_cast<ExclusionSet*>(ih);
  delete ri;
}


///////////////////////////////////////////////////////////////////////////////
//// This is the main "entry" point for the plugin, called for every request.
////
TSRemapStatus
TSRemapDoRemap(void* ih, TSHttpTxn txnp, TSRemapRequestInfo* /* rri */)
{
  if (NULL == ih) {
    return TSREMAP_NO_REMAP;
  }

  TSDebug(PLUGIN_NAME, "background fetch TSRemapDoRemap...");
  TSHttpTxnArgSet(txnp, g_background_fetch_ArgIndex, static_cast<void *> (ih));
  TSHttpTxnHookAdd(txnp, TS_HTTP_READ_RESPONSE_HDR_HOOK, TSContCreate(cont_handle_response, NULL));

  return TSREMAP_NO_REMAP;
}
