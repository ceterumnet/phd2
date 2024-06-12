#include "json.hpp"
#include "phd.h"
#include <arpa/inet.h>
#include <bits/types/struct_timeval.h>
#include <curl/curl.h>
#include <curl/easy.h>
#include <netinet/in.h>
#include <string>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <thread>
#ifdef GUIDE_ALPACA

static std::map<std::string, std::string> alpaca_telescopes;

ScopeAlpaca::ScopeAlpaca(const wxString &choice)
    : m_canPulseGuide(false), m_canSlew(false), m_canSlewAsync(false),
      m_choice(choice), _client_id(8675309), _client_transaction_id(1) {

  alpaca_base_url = alpaca_telescopes[std::string(choice)];
  Debug.Write(wxString::Format("Alpaca Scope: base_url - %s\n", alpaca_base_url));
  // Doing this with string concatenation is not "ideal", but it works for now.
  _connect_url = alpaca_base_url + "/connected";
  _get_guide_rate_dec_url = alpaca_base_url + "/guideratedeclination";
  _get_guide_rate_ra_url = alpaca_base_url + "/guideraterightascension";
  _get_ra_url = alpaca_base_url + "/rightascension";
  _get_dec_url = alpaca_base_url + "/declination";
  _get_site_lat_url = alpaca_base_url + "/sitelatitude";
  _get_site_lon_url = alpaca_base_url + "/sitelongitude";
  _get_can_slew_url = alpaca_base_url + "/canslew";
  _get_can_slew_async_url = alpaca_base_url + "/canslewasync";
  _get_can_pulse_guide_url = alpaca_base_url + "/canpulseguide";
  _get_side_of_pier_url = alpaca_base_url + "/sideofpier";
  _get_sidereal_time_url = alpaca_base_url + "/siderealtime";
  _slew_to_coordinates_url = alpaca_base_url + "/slewtocoordinates";
  _slew_to_coordinates_async_url = alpaca_base_url + "/slewtocoordinatesasync";
  _abort_slew_url = alpaca_base_url + "/abortslew";
  _slewing_url = alpaca_base_url + "/slewing";
  _pulse_guide_url = alpaca_base_url + "/pulseguide";

  // This is global across all of the program. Other parts of PHD2
  // invoke this call. Based on the curl
  // documentation this is safe to call multiple times.
  curl_global_init(CURL_GLOBAL_DEFAULT);
  _curl_handle = curl_easy_init();
  _curl_headers =
      curl_slist_append(_curl_headers, "Content-Type: application/json");

  // I'm not sure we want to do this in the CTOR or have this in some other
  // initialization code that happens
  if (!_curl_handle) {
    // throw exception here possibly if curl fails to init for some reason
    throw ERROR_INFO("Failed to initialize curl");
  }
}

ScopeAlpaca::~ScopeAlpaca() {
  curl_slist_free_all(_curl_headers);
  curl_easy_cleanup(_curl_handle);
}

static size_t __curl_write_cb(void *contents, size_t size, size_t nmemb,
                              void *userp) {
  size_t data_size = size * nmemb;
  auto mem = static_cast<std::vector<char> *>(userp);
  // +1 for null terminator
  mem->resize(data_size + 1);
  Debug.Write(wxString::Format("Data size from curl: %d\n", nmemb));
  Debug.Write(wxString::Format("Vector size after resize: %d\n", mem->size()));
  mem->assign(static_cast<char *>(contents),
              static_cast<char *>(contents) + data_size);
  mem->data()[data_size] = '\0';
  return data_size;
}


// TODO: either implement discovery or some other means to
// add the list of scopes with the config dialog for example
wxArrayString ScopeAlpaca::EnumAlpacaScopes() {
  wxArrayString list;
  Debug.Write(wxString::Format("Alpaca Scope: Enumerating available telescopes\n"));

  // create a broadcast socket
  sockaddr_in si_me;
  sockaddr_in si_server;

  int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

  if (s == -1) {
    std::cout << "EnumAlpacaScopes: failed to create socket" << std::endl;
    Debug.Write(wxString::Format("Alpaca Scope: Failed to create discovery socket\n"));
    return list;
  }

  // default alpaca discovery port
  int port = 32227;

  int broadcast = 1;
  setsockopt(s, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof broadcast);

  memset(&si_me, 0, sizeof(si_me));
  si_me.sin_family = AF_INET;
  si_me.sin_port = htons(port);
  si_me.sin_addr.s_addr = INADDR_BROADCAST;

  std::string discovery_msg = "alpacadiscovery1";

  Debug.Write(wxString::Format("Alpaca Scope: Sending discovery message\n"));
  socklen_t len;
  sendto(s, discovery_msg.data(), discovery_msg.length(), 0,
         (const sockaddr *)&si_me, sizeof(si_me));
  Debug.Write(wxString::Format("Discovery message sent. Waiting for reply\n"));

  std::string rsp_buf;
  rsp_buf.resize(1024);
  struct sockaddr_in Recv_addr;

  int ret = 0;
  socklen_t r_len;
  int flags = 0;
  int count = 0;
  int addr_len = sizeof(struct sockaddr_in);
  std::string discovery_url = "http://";

  struct timeval tv;
  tv.tv_sec = 0;
  tv.tv_usec = 250000;
  if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
    Debug.Write(
        wxString::Format("Alpaca Scope: Error setting socket timeout\n"));
    return list;
  }

  CURLcode curl_res;
  curl_global_init(CURL_GLOBAL_DEFAULT);
  CURL *curl_handle = curl_easy_init();
  struct curl_slist *curl_headers = NULL;
  if (!curl_handle) {
    return list;
  }

  curl_headers =
      curl_slist_append(curl_headers, "Content-Type: application/json");

  // try a few times since it's UDP broadcast and we may have multiple alpaca
  // servers answering
  for (int i = 0; i < 5; i++) {
    memset(&si_server, 0, sizeof(si_server));
    si_me.sin_family = AF_INET;
    si_server.sin_addr.s_addr = INADDR_ANY;
    count = recvfrom(s, &rsp_buf[0], 1024, flags, (struct sockaddr *)&si_server,
                     &r_len);
    if (count > 0) {
      std::string i_addr_str;
      i_addr_str.resize(r_len + 1);
      inet_ntop(AF_INET, &si_server.sin_addr, &i_addr_str[0], r_len);
      if (rsp_buf.find("Alpaca") != std::string::npos) {
        Debug.Write(
            wxString::Format("Alpaca Scope: Received Alpaca Discovery Response\n"));

        try {
          auto parsed_discovery_msg = nlohmann::json::parse(rsp_buf);
          int alpaca_port = parsed_discovery_msg["AlpacaPort"];
          auto url_to_add =
              discovery_url + std::string(inet_ntoa(si_server.sin_addr)) + ":" +
              std::to_string(alpaca_port) + "/management/v1/configureddevices";

          std::vector<char> curl_buf;
          curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, curl_headers);
          curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, __curl_write_cb);
          curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&curl_buf);
          curl_easy_setopt(curl_handle, CURLOPT_URL, url_to_add.c_str());
          curl_res = curl_easy_perform(curl_handle);

          if (curl_res != CURLE_OK) {
            Debug.Write(
                wxString::Format("Alpaca Scope: Problem fetching from %s\n",
                                 discovery_url.data()));
          } else {
            Debug.Write(
                wxString::Format("Alpaca Scope: Data returned: %s\n", curl_buf.data()));
            auto parsed_res = nlohmann::json::parse(curl_buf.data());
            for (auto item : parsed_res["Value"]) {
              if (item["DeviceType"] == "telescope") {
                std::string displ_str = "Alpaca Telescope - " +
                                        item["DeviceName"].get<std::string>();
                // Let's just store the base url for the device since that is
                // how we should access it
                alpaca_telescopes[displ_str] =
                    discovery_url + std::string(inet_ntoa(si_server.sin_addr)) +
                    ":" + std::to_string(alpaca_port) + "/api/v1/telescope/" +
                    std::to_string(item["DeviceNumber"].get<int>());
                Debug.Write(wxString::Format(
                    "Alpaca Scope: Adding %s to list\n"));
                list.Add(wxString(displ_str));
              }
            }
          }
        } catch (std::exception &ex) {
          Debug.Write(
            wxString::Format("Alpaca Scope: Problem parsing discovery payload\n"));
        }
      }
    }
  };

  // cleanup curl
  curl_slist_free_all(curl_headers);
  curl_easy_cleanup(curl_handle);

  return list;
}

int ScopeAlpaca::get_next_client_transaction_id() {
  return _client_transaction_id++;
}

// Still thinking through the structure of the curl code
// I don't know if I need this or not
void ScopeAlpaca::set_curl_opts() {}

void ScopeAlpaca::invoke_curl_put(const std::string &url,
                                  const std::string &put_data,
                                  const std::vector<char> &curl_buf) {
  curl_easy_reset(_curl_handle);
  // first, we need to connect

  curl_easy_setopt(_curl_handle, CURLOPT_HTTPHEADER, _curl_headers);
  curl_easy_setopt(_curl_handle, CURLOPT_URL, url.c_str());
  curl_easy_setopt(_curl_handle, CURLOPT_POSTFIELDS, put_data.data());
  curl_easy_setopt(_curl_handle, CURLOPT_CUSTOMREQUEST, "PUT");
  curl_easy_setopt(_curl_handle, CURLOPT_WRITEDATA, (void *)&curl_buf);
  curl_easy_setopt(_curl_handle, CURLOPT_WRITEFUNCTION, __curl_write_cb);

  _curl_res = curl_easy_perform(_curl_handle);
}

void ScopeAlpaca::invoke_curl_get(const std::string &url,
                                  const std::vector<char> &curl_buf) {
  curl_easy_reset(_curl_handle);
  curl_easy_setopt(_curl_handle, CURLOPT_HTTPHEADER, _curl_headers);
  curl_easy_setopt(_curl_handle, CURLOPT_WRITEFUNCTION, __curl_write_cb);
  curl_easy_setopt(_curl_handle, CURLOPT_WRITEDATA, (void *)&curl_buf);

  auto the_url = wxString::Format("%s?ClientTransactionID=%d&ClientID=%d", url,
                                  get_next_client_transaction_id(), _client_id);

  curl_easy_setopt(_curl_handle, CURLOPT_URL, the_url.ToUTF8().data());
  _curl_res = curl_easy_perform(_curl_handle);

  if (_curl_res != CURLE_OK)
    ERROR_INFO("Alpaca Scope: Problem with fetching");
  else
    Debug.Write(wxString::Format("Data returned: %s\n", curl_buf.data()));
}

bool ScopeAlpaca::Connect() {
  std::vector<char> curl_buf;
  auto put_data =
      wxString::Format("Connected=True&ClientTransactionID=%d&ClientID=%d",
                       get_next_client_transaction_id(), _client_id);
  invoke_curl_put(_connect_url, std::string(put_data.ToUTF8()), curl_buf);

  if (_curl_res != CURLE_OK) {
    ERROR_INFO("Alpaca Scope: Problem with connect call");
    return true;
  }

  try {
    auto parsed_resp = nlohmann::json::parse(curl_buf.data());
    if (parsed_resp["ErrorNumber"] != 0)
      return true;
  } catch (std::exception &ex) {
    return true;
  }

  Debug.Write(wxString::Format("Alpaca Mount: Connected successfully.\n"));

  // let's loop through the "can_*" calls and set the members accordingly
  std::map<std::string, bool &> can_list{
      {_get_can_slew_url, m_canSlew},
      {_get_can_slew_async_url, m_canSlewAsync},
      {_get_can_pulse_guide_url, m_canPulseGuide}};

  for (auto can_item : can_list) {
    try {
      Debug.Write(
          wxString::Format("Alpaca Mount: Fetching %s\n", can_item.first));
      invoke_curl_get(can_item.first, curl_buf);
      if (_curl_res == CURLE_OK) {
        auto parsed_res = nlohmann::json::parse(curl_buf.data());
        if (parsed_res["ErrorNumber"] != 0) {
          Debug.Write(wxString::Format("Alpaca Scope: Problem fetching %s\n",
                                       can_item.first));
          return true;
        }
        can_item.second = parsed_res["Value"];
      } else {
        ERROR_INFO("Alpaca Scope: Problem fetching value");
      }
    } catch (std::exception &ex) {
      // ERROR_INFO(ex.what());
      Debug.Write(wxString::Format("Alpaca Scope: Exception %s\n", ex.what()));
      ERROR_INFO("Alpaca Scope: Exception thrown");
    }
  }

  Debug.Write(wxString::Format("m_canSlew %d\n", m_canSlew));
  Debug.Write(wxString::Format("m_canSlewAsync %d\n", m_canSlewAsync));
  Debug.Write(wxString::Format("m_canPulseGuide %d\n", m_canPulseGuide));

  Scope::Connect();
  return false;
}

bool ScopeAlpaca::Disconnect() {
  std::vector<char> curl_buf;
  auto put_data =
      wxString::Format("Connected=False&ClientTransactionID=%d&ClientID=%d",
                       get_next_client_transaction_id(), _client_id);

  invoke_curl_put(_connect_url, std::string(put_data.ToUTF8()), curl_buf);

  if (_curl_res != CURLE_OK) {
    ERROR_INFO("Alpaca Scope: Problem with disconnect call");
    return true;
  }

  try {
    auto parsed_resp = nlohmann::json::parse(curl_buf.data());
    if (parsed_resp["ErrorNumber"] != 0)
      return true;
  } catch (std::exception &ex) {
    return true;
  }

  Debug.Write(wxString::Format("Alpaca Mount: Disconnected successfully.\n"));

  Scope::Disconnect();
  return false;
}

// Need to figure out if this will have a setup dialog or
// not. I'm guessing I might just display a simple link that
// will open the web based config. Need to think about it
bool ScopeAlpaca::HasSetupDialog() const { return false; }

void ScopeAlpaca::SetupDialog(){};

// Need to look at what this means exactly
bool ScopeAlpaca::HasNonGuiMove() { return true; };

Mount::MOVE_RESULT ScopeAlpaca::Guide(GUIDE_DIRECTION direction, int duration) {
  std::vector<char> curl_buf;
  MOVE_RESULT result = MOVE_OK;
  int direction_i = 0;
  switch (direction) {
  case GUIDE_DIRECTION::SOUTH:
    direction_i = 1;
    break;
  case GUIDE_DIRECTION::NORTH:
    direction_i = 0;
    break;
  case GUIDE_DIRECTION::EAST:
    direction_i = 2;
    break;
  case GUIDE_DIRECTION::WEST:
    direction_i = 3;
    break;
  default:
    return MOVE_RESULT::MOVE_ERROR;
  }

  auto put_data = wxString::Format(
      "Direction=%d&Duration=%d&ClientTransactionID=%d&ClientID=%d",
      direction_i, duration, get_next_client_transaction_id(), _client_id);

  // invoke_curl_put(_pulse_guide_url, put_data, )
  return result;
}

double ScopeAlpaca::GetDeclinationRadians() {
  std::vector<char> curl_buf;
  double dReturn = UNKNOWN_DECLINATION;

  invoke_curl_get(_get_dec_url, curl_buf);

  if (_curl_res != CURLE_OK) {
    ERROR_INFO("Alpaca Scope: Problem with get declination call");
    return true;
  }

  try {
    auto parsed_res = nlohmann::json::parse(curl_buf.data());
    if (parsed_res["ErrorNumber"] != 0)
      return true;
    dReturn = parsed_res["Value"];
    return false;
  } catch (std::exception &ex) {
    Debug.Write(wxString::Format(
        "Alpaca Mount: Problem fetching declination: %s\n", ex.what()));
  }

  Debug.Write(
      wxString::Format("Alpaca Mount: Declination fetched succesfully.\n"));

  return dReturn;
}

bool ScopeAlpaca::GetGuideRates(double *pRAGuideRate, double *pDecGuideRate) {
  std::vector<char> curl_buf;
  *pRAGuideRate = .5;
  *pDecGuideRate = .5;

  invoke_curl_get(_get_guide_rate_dec_url, curl_buf);

  if (_curl_res != CURLE_OK) {
    ERROR_INFO("Alpaca Scope: Problem with get guide rate declination call");
    return true;
  }

  try {
    auto parsed_res = nlohmann::json::parse(curl_buf.data());
    if (parsed_res["ErrorNumber"] != 0)
      return true;
    *pDecGuideRate = parsed_res["Value"];
  } catch (std::exception &ex) {
    Debug.Write(wxString::Format(
        "Alpaca Mount: Problem fetching declination: %s\n", ex.what()));
    return true;
  }

  Debug.Write(wxString::Format(
      "Alpaca Mount: Declination guide rate fetched succesfully.\n"));

  invoke_curl_get(_get_guide_rate_ra_url, curl_buf);

  if (_curl_res != CURLE_OK) {
    ERROR_INFO(
        "Alpaca Scope: Problem with get guide rate right ascension call");
    return true;
  }

  try {
    auto parsed_res = nlohmann::json::parse(curl_buf.data());
    if (parsed_res["ErrorNumber"] != 0)
      return true;
    *pRAGuideRate = parsed_res["Value"];
  } catch (std::exception &ex) {
    Debug.Write(wxString::Format(
        "Alpaca Mount: Problem fetching RA guide rate: %s\n", ex.what()));
    return true;
  }

  Debug.Write(wxString::Format(
      "Alpaca Mount: Right ascension guide rate fetched succesfully.\n"));

  return false;
}

bool ScopeAlpaca::GetCoordinates(double *ra, double *dec,
                                 double *siderealTime) {
  std::vector<char> curl_buf;
  invoke_curl_get(_get_ra_url, curl_buf);

  if (_curl_res != CURLE_OK) {
    ERROR_INFO("Alpaca Scope: Problem with get right ascension call");
    return true;
  }

  Debug.Write(wxString::Format(
      "Alpaca Mount: Right ascension guide rate fetched succesfully.\n"));

  try {
    auto parsed_res = nlohmann::json::parse(curl_buf.data());
    if (parsed_res["ErrorNumber"] != 0)
      return true;
    *ra = parsed_res["Value"];
  } catch (std::exception &ex) {
    Debug.Write(wxString::Format(
        "Alpaca Mount: Problem fetching RA guide rate: %s\n", ex.what()));
    return true;
  }

  invoke_curl_get(_get_dec_url, curl_buf);

  if (_curl_res != CURLE_OK) {
    ERROR_INFO("Alpaca Scope: Problem with get declination call");
    return true;
  }

  Debug.Write(wxString::Format(
      "Alpaca Mount: Right ascension guide rate fetched succesfully.\n"));

  try {
    auto parsed_res = nlohmann::json::parse(curl_buf.data());
    if (parsed_res["ErrorNumber"] != 0)
      return true;
    *dec = parsed_res["Value"];
  } catch (std::exception &ex) {
    Debug.Write(wxString::Format(
        "Alpaca Mount: Problem fetching RA guide rate: %s\n", ex.what()));
    return true;
  }

  invoke_curl_get(_get_sidereal_time_url, curl_buf);

  if (_curl_res != CURLE_OK) {
    ERROR_INFO("Alpaca Scope: Problem with get sidereal time call");
    return true;
  }

  Debug.Write(
      wxString::Format("Alpaca Mount: Sidereal Time fetched succesfully.\n"));

  try {
    auto parsed_res = nlohmann::json::parse(curl_buf.data());
    if (parsed_res["ErrorNumber"] != 0)
      return true;

    *siderealTime = parsed_res["Value"];

  } catch (std::exception &ex) {
    Debug.Write(wxString::Format(
        "Alpaca Mount: Problem fetching sidereal time: %s\n", ex.what()));
    return true;
  }

  return false;
}

bool ScopeAlpaca::GetSiteLatLong(double *latitude, double *longitude) {
  std::vector<char> curl_buf;
  invoke_curl_get(_get_site_lat_url, curl_buf);

  if (_curl_res != CURLE_OK) {
    ERROR_INFO("Alpaca Scope: Problem with get site latitude call");
    return true;
  }

  Debug.Write(
      wxString::Format("Alpaca Mount: Latitude fetched succesfully.\n"));

  try {
    auto parsed_res = nlohmann::json::parse(curl_buf.data());
    if (parsed_res["ErrorNumber"] != 0)
      return true;
    *latitude = parsed_res["Value"];

  } catch (std::exception &ex) {
    Debug.Write(wxString::Format(
        "Alpaca Mount: Problem fetching latitude: %s\n", ex.what()));
    return true;
  }

  invoke_curl_get(_get_site_lon_url, curl_buf);

  if (_curl_res != CURLE_OK) {
    ERROR_INFO("Alpaca Scope: Problem with get site longitude call");
    return true;
  }

  Debug.Write(
      wxString::Format("Alpaca Mount: Longitude fetched succesfully.\n"));

  try {
    auto parsed_res = nlohmann::json::parse(curl_buf.data());
    if (parsed_res["ErrorNumber"] != 0)
      return true;

    *longitude = parsed_res["Value"];

  } catch (std::exception &ex) {
    Debug.Write(wxString::Format(
        "Alpaca Mount: Problem fetching longitude: %s\n", ex.what()));
    return true;
  }

  return false;
}

bool ScopeAlpaca::CanSlew() {
  try {
    if (!IsConnected()) {
      throw ERROR_INFO(
          "Alpaca Scope: Cannot get CanSlew when scope is not connected. ");
    }
  } catch (const wxString &msg) {
    POSSIBLY_UNUSED(msg);
    return false;
  }

  return m_canSlew;
}

bool ScopeAlpaca::CanSlewAsync() {
  try {
    if (!IsConnected()) {
      throw ERROR_INFO("Alpaca Scope: Cannot get CanSlewAsync when scope is "
                       "not connected. ");
    }
  } catch (const wxString &msg) {
    POSSIBLY_UNUSED(msg);
    return false;
  }
  return m_canSlewAsync;
}

bool ScopeAlpaca::CanReportPosition() { return true; }

bool ScopeAlpaca::CanPulseGuide() { return m_canPulseGuide; }

bool ScopeAlpaca::SlewToCoordinates(double ra, double dec) {
  std::vector<char> curl_buf;
  auto put_data = wxString::Format(
      "RightAscension=%d&Declination=%d&ClientTransactionID=%d&ClientID=%d", ra,
      dec, get_next_client_transaction_id(), _client_id);
  invoke_curl_put(_slew_to_coordinates_url, std::string(put_data.ToUTF8()),
                  curl_buf);

  if (_curl_res != CURLE_OK) {
    ERROR_INFO("Alpaca Scope: Problem with slew to coordinates call");
    return true;
  }

  try {
    auto parsed_res = nlohmann::json::parse(curl_buf.data());
    if (parsed_res["ErrorNumber"] != 0)
      return true;
  } catch (std::exception &ex) {
    Debug.Write(wxString::Format(
        "Alpaca Mount: Failed to slew to coordinates. %s\n", ex.what()));
  }

  Debug.Write(
      wxString::Format("Alpaca Mount: Slew To Coordinates successful.\n"));

  return false;
}

bool ScopeAlpaca::SlewToCoordinatesAsync(double ra, double dec) {
  std::vector<char> curl_buf;
  auto put_data = wxString::Format(
      "RightAscension=%d&Declination=%d&ClientTransactionID=%d&ClientID=%d", ra,
      dec, get_next_client_transaction_id(), _client_id);
  invoke_curl_put(_slew_to_coordinates_async_url,
                  std::string(put_data.ToUTF8()), curl_buf);

  if (_curl_res != CURLE_OK) {
    ERROR_INFO("Alpaca Scope: Problem with slew to coordinates async call");
    return true;
  }

  try {
    auto parsed_res = nlohmann::json::parse(curl_buf.data());
    if (parsed_res["ErrorNumber"] != 0)
      return true;
  } catch (std::exception &ex) {
    Debug.Write(wxString::Format(
        "Alpaca Mount: Failed to slew to coordinates async. %s\n", ex.what()));
  }

  Debug.Write(wxString::Format("Alpaca Mount: Connected successfully.\n"));

  return false;
}

void ScopeAlpaca::AbortSlew() {
  std::vector<char> curl_buf;
  auto put_data =
      wxString::Format("ClientTransactionID=%d&ClientID=%d",
                       get_next_client_transaction_id(), _client_id);
  invoke_curl_put(_abort_slew_url, std::string(put_data.ToUTF8()), curl_buf);

  if (_curl_res != CURLE_OK) {
    ERROR_INFO("Alpaca Scope: Problem with abort slew call");
  }
  try {
    auto parsed_res = nlohmann::json::parse(curl_buf.data());
    if (parsed_res["ErrorNumber"] != 0)
      Debug.Write(
          wxString::Format("Alpaca Mount: Problem aborting slewing: %s\n",
                           std::string(parsed_res["ErrorMessage"])));

  } catch (std::exception &ex) {
    Debug.Write(wxString::Format("Alpaca Mount: Problem fetching slewing: %s\n",
                                 ex.what()));
  }

  Debug.Write(wxString::Format("Alpaca Mount: Abort Slew successful.\n"));
}

bool ScopeAlpaca::CanCheckSlewing() { return true; }

bool ScopeAlpaca::Slewing() {
  std::vector<char> curl_buf;
  invoke_curl_get(_slewing_url, curl_buf);

  if (_curl_res != CURLE_OK) {
    ERROR_INFO("Alpaca Scope: Problem with slewing call");
  }

  Debug.Write(wxString::Format("Alpaca Mount: Slewing fetched succesfully.\n"));
  bool is_slewing = false;
  try {
    auto parsed_res = nlohmann::json::parse(curl_buf.data());
    is_slewing = parsed_res["Value"];
  } catch (std::exception &ex) {
    Debug.Write(wxString::Format("Alpaca Mount: Problem fetching slewing: %s\n",
                                 ex.what()));
  }

  return is_slewing;
}

PierSide ScopeAlpaca::SideOfPier() {
  std::vector<char> curl_buf;
  invoke_curl_get(_get_side_of_pier_url, curl_buf);

  if (_curl_res != CURLE_OK) {
    ERROR_INFO("Alpaca Scope: Problem with side of pier call");
  }

  Debug.Write(wxString::Format("Alpaca Mount: Side of pier succesfully.\n"));

  try {
    auto parsed_res = nlohmann::json::parse(curl_buf.data());
    auto side_of_pier_value = parsed_res["Value"];

    if (side_of_pier_value == 0)
      return PierSide::PIER_SIDE_EAST;
    if (side_of_pier_value == 1)
      return PierSide::PIER_SIDE_WEST;

  } catch (std::exception &ex) {
    Debug.Write(wxString::Format(
        "Alpaca Mount: Problem fetching sidereal time: %s\n", ex.what()));
  }

  return PierSide::PIER_SIDE_UNKNOWN;
}

#endif // GUIDE_ALPACA
