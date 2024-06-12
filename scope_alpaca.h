#ifndef SCOPE_ALPACA_INCLUDED
#define SCOPE_ALPACA_INCLUDED

#ifdef GUIDE_ALPACA

#include <curl/curl.h>

class ScopeAlpaca : public Scope {
  // other private variables
  bool m_canCheckPulseGuiding;
  bool m_canGetCoordinates;
  bool m_canGetGuideRates;
  bool m_canSlew;
  bool m_canSlewAsync;
  bool m_canPulseGuide;

  bool m_abortSlewWhenGuidingStuck;
  bool m_checkForSyncPulseGuide;

  bool _connected;

  std::string alpaca_base_url;

  wxString m_choice; // name of chosen scope

  std::string _connect_url;
  std::string _get_guide_rate_dec_url;
  std::string _get_guide_rate_ra_url;
  std::string _get_ra_url;
  std::string _get_dec_url;
  std::string _get_site_lat_url;
  std::string _get_site_lon_url;
  std::string _get_can_slew_url;
  std::string _get_can_slew_async_url;
  std::string _get_can_pulse_guide_url;
  std::string _get_side_of_pier_url;
  std::string _get_sidereal_time_url;
  std::string _slew_to_coordinates_url;
  std::string _slew_to_coordinates_async_url;
  std::string _abort_slew_url;
  std::string _slewing_url;
  std::string _pulse_guide_url;

  int _client_transaction_id;
  int get_next_client_transaction_id();
  int _client_id;
  // libcurl members
  CURL *_curl_handle;
  void set_curl_opts();
  void invoke_curl_get(const std::string &get_url, const std::vector<char> &curl_buf);
  void invoke_curl_put(const std::string &put_url, const std::string &put_body,
                       const std::vector<char> &curl_buf);

  CURLcode _curl_res;
  // std::vector<char> _curl_mem;
  struct curl_slist *_curl_headers = NULL;

  // std::string basic_http_get(std::string url, )
public:
  ScopeAlpaca(const wxString &choice);
  virtual ~ScopeAlpaca();
  static wxArrayString EnumAlpacaScopes();

  bool Connect() override;
  bool Disconnect() override;

  bool HasSetupDialog() const override;
  void SetupDialog() override;

  bool HasNonGuiMove() override;

  MOVE_RESULT Guide(GUIDE_DIRECTION direction, int durationMs) override;

  double GetDeclinationRadians() override;
  bool GetGuideRates(double *pRAGuideRate, double *pDecGuideRate) override;
  bool GetCoordinates(double *ra, double *dec, double *siderealTime) override;
  bool GetSiteLatLong(double *latitude, double *longitude) override;
  bool CanSlew() override;
  bool CanSlewAsync() override;
  bool CanReportPosition() override;
  bool CanPulseGuide() override;
  bool SlewToCoordinates(double ra, double dec) override;
  bool SlewToCoordinatesAsync(double ra, double dec) override;
  void AbortSlew() override;
  bool CanCheckSlewing() override;
  bool Slewing() override;
  PierSide SideOfPier() override;
};

#endif // GUIDE_ALPACA
#endif // SCOPE_ALPACA_INCLUDED
