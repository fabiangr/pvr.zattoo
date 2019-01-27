#include <iostream>
#include <string>
#include "ZatData.h"
#include <sstream>
#include "p8-platform/sockets/tcp.h"
#include <map>
#include <ctime>
#include <random>
#include <utility>
#include "Utils.h"
#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"
#include "Cache.h"
#include "md5.h"

#ifdef TARGET_ANDROID
#include "to_string.h"
#endif

using namespace ADDON;
using namespace std;
using namespace rapidjson;

constexpr char app_token_file[] = "special://temp/zattoo_app_token";
const char data_file[] = "special://profile/addon_data/pvr.zattoo/data.json";
static const std::string user_agent = string("Kodi/") + string(STR(KODI_VERSION)) + string(" pvr.zattoo/") + string(STR(ZATTOO_VERSION)) + string(" (Kodi PVR addon)");

string ZatData::HttpGetCached(const string& url, time_t cacheDuration, const string& userAgent)
{

  string content;
  string cacheKey = md5(url);
  if (!Cache::Read(cacheKey, content))
  {
    content = HttpGet(url, false, userAgent);
    if (!content.empty())
    {
      time_t validUntil;
      time(&validUntil);
      validUntil += cacheDuration;
      Cache::Write(cacheKey, content, validUntil);
    }
  }
  return content;
}

string ZatData::HttpGet(const string& url, bool isInit, const string& userAgent)
{
  return HttpRequest("GET", url, "", isInit, userAgent);
}

string ZatData::HttpDelete(const string& url, bool isInit)
{
  return HttpRequest("DELETE", url, "", isInit, "");
}

string ZatData::HttpPost(const string& url, const string& postData, bool isInit, const string& userAgent)
{
  return HttpRequest("POST", url, postData, isInit, userAgent);
}

string ZatData::HttpRequest(const string& action, const string& url, const string& postData,
                            bool isInit, const string& userAgent)
{
  Curl curl;
  int statusCode;
  
  curl.AddOption("acceptencoding", "gzip,deflate");

  if (!beakerSessionId.empty())
  {
    curl.AddOption("cookie", "beaker.session.id=" + beakerSessionId);
  }

  if (!pzuid.empty())
  {
    curl.AddOption("Cookie", "pzuid=" + pzuid);
  }

  if (!userAgent.empty()) {
    curl.AddHeader("User-Agent", userAgent);
  }

  string content = HttpRequestToCurl(curl, action, url, postData, statusCode);

  if (statusCode == 403 && !isInit)
  {
    XBMC->Log(LOG_ERROR, "Open URL failed. Try to re-init session.");
    if (!InitSession())
    {
      XBMC->Log(LOG_ERROR, "Re-init of session. Failed.");
      return "";
    }
    return HttpRequestToCurl(curl, action, url, postData, statusCode);
  }
  string sessionId = curl.GetCookie("beaker.session.id");
  if (!sessionId.empty() && beakerSessionId != sessionId)
  {
    XBMC->Log(LOG_DEBUG, "Got new beaker.session.id: %s..", sessionId.substr(0, 5).c_str());
    beakerSessionId = sessionId;
  }
  
  string pzuid = curl.GetCookie("pzuid");
  if (!pzuid.empty() && this->pzuid != pzuid)
  {
    XBMC->Log(LOG_DEBUG, "Got new pzuid: %s..", pzuid.substr(0,5).c_str());
    this->pzuid = pzuid;
    WriteDataJson();
  }

  return content;
}

string ZatData::HttpRequestToCurl(Curl &curl, const string& action, const string& url,
                                  const string& postData, int &statusCode)
{
  XBMC->Log(LOG_DEBUG, "Http-Request: %s %s.", action.c_str(), url.c_str());
  string content;
  if (action == "POST")
  {
    content = curl.Post(url, postData, statusCode);
  }
  else if (action == "DELETE")
  {
    content = curl.Delete(url, statusCode);
  }
  else
  {
    content = curl.Get(url, statusCode);
  }
  return content;

}

bool ZatData::ReadDataJson()
{
  if (!XBMC->FileExists(data_file, true))
  {
    return true;
  }
  string jsonString = Utils::ReadFile(data_file);
  if (jsonString.empty())
  {
    XBMC->Log(LOG_ERROR, "Loading data.json failed.");
    return false;
  }

  Document doc;
  doc.Parse(jsonString.c_str());
  if (doc.GetParseError())
  {
    XBMC->Log(LOG_ERROR, "Parsing data.json failed.");
    return false;
  }

  const Value& recordings = doc["recordings"];
  for (Value::ConstValueIterator itr = recordings.Begin();
      itr != recordings.End(); ++itr)
  {
    const Value& recording = (*itr);
    auto *recData = new ZatRecordingData();
    recData->recordingId = GetStringOrEmpty(recording, "recordingId");
    recData->playCount = recording["playCount"].GetInt();
    recData->lastPlayedPosition = recording["lastPlayedPosition"].GetInt();
    recData->stillValid = false;
    recordingsData[recData->recordingId] = recData;
  }
  
  recordingsLoaded = false;

  if (doc.HasMember("pzuid"))
  {
    pzuid = GetStringOrEmpty(doc, "pzuid");
    XBMC->Log(LOG_DEBUG, "Loaded pzuid: %s..", pzuid.substr(0,5).c_str());
  }
  
  if (doc.HasMember("uuid"))
  {
    uuid = GetStringOrEmpty(doc, "uuid");
    XBMC->Log(LOG_DEBUG, "Loaded uuid: %s", uuid.c_str());
  }
  
  XBMC->Log(LOG_DEBUG, "Loaded data.json.");
  return true;
}

bool ZatData::WriteDataJson()
{
  void* file;
  if (!(file = XBMC->OpenFileForWrite(data_file, true)))
  {
    XBMC->Log(LOG_ERROR, "Save data.json failed.");
    return false;
  }

  Document d;
  d.SetObject();

  Value a(kArrayType);
  Document::AllocatorType& allocator = d.GetAllocator();
  for (auto const& item : recordingsData)
  {
    if (recordingsLoaded &&  !item.second->stillValid)
    {
      continue;
    }

    Value r;
    r.SetObject();
    Value recordingId;
    recordingId.SetString(item.second->recordingId.c_str(),
        item.second->recordingId.length(), allocator);
    r.AddMember("recordingId", recordingId, allocator);
    r.AddMember("playCount", item.second->playCount, allocator);
    r.AddMember("lastPlayedPosition", item.second->lastPlayedPosition,
        allocator);
    a.PushBack(r, allocator);
  }
  d.AddMember("recordings", a, allocator);
  
  if (!pzuid.empty()) {
    Value pzuidValue;
    pzuidValue.SetString(pzuid.c_str(),
        pzuid.length(), allocator);
    d.AddMember("pzuid", pzuidValue, allocator);
  }
  
  if (!uuid.empty()) {
    Value uuidValue;
    uuidValue.SetString(uuid.c_str(),
        uuid.length(), allocator);
    d.AddMember("uuid", uuidValue, allocator);
  }

  StringBuffer buffer;
  Writer<StringBuffer> writer(buffer);
  d.Accept(writer);
  const char* output = buffer.GetString();
  XBMC->WriteFile(file, output, strlen(output));
  XBMC->CloseFile(file);
  return true;
}

string ZatData::GetUUID()
{
  if (!uuid.empty())
  {
    return uuid;
  }

  uuid = GenerateUUID();
  WriteDataJson();
  return uuid;
}

string ZatData::GenerateUUID()
{
  random_device rd;
  mt19937 rng(rd());
  uniform_int_distribution<int> uni(0, 15);
  ostringstream uuid;

  uuid << hex;

  for (int i = 0; i < 32; i++)
  {
    if (i == 8 || i == 12 || i == 16 || i == 20)
    {
      uuid << "-";
    }
    if (i == 12)
    {
      uuid << 4;
    }
    else if (i == 16)
    {
      uuid << ((uni(rng) % 4) + 8);
    }
    else
    {
      uuid << uni(rng);
    }
  }
  return uuid.str();
}

bool ZatData::LoadAppId()
{
  string html = HttpGet(providerUrl, true);

  appToken = "";
  //There seems to be a problem with old gcc and osx with regex. Do it the dirty way:
  size_t basePos = html.find("window.appToken = '") + 19;
  if (basePos > 19)
  {
    size_t endPos = html.find("'", basePos);
    appToken = html.substr(basePos, endPos - basePos);
    
    void* file;
    if (!(file = XBMC->OpenFileForWrite(app_token_file, true)))
    {
      XBMC->Log(LOG_ERROR, "Could not save app taken to %s", app_token_file);
    }
    else
    {
      XBMC->WriteFile(file, appToken.c_str(), appToken.length());
      XBMC->CloseFile(file);
    }
  }

  if (appToken.empty() && XBMC->FileExists(app_token_file, true))
  {
    XBMC->Log(LOG_NOTICE, "Could not get app token from page. Try to load from file.");
    appToken = Utils::ReadFile(app_token_file);
  }
  
  if (appToken.empty())
  {
    XBMC->Log(LOG_ERROR, "Unable to get app token.");
    return false;
  }

  XBMC->Log(LOG_DEBUG, "Loaded app token %s", appToken.c_str());
  return true;

}

bool ZatData::SendHello(string uuid)
{
  XBMC->Log(LOG_DEBUG, "Send hello.");
  ostringstream dataStream;
  dataStream << "uuid=" << uuid << "&lang=en&format=json&client_app_token="
      << appToken;

  string jsonString = HttpPost(providerUrl + "/zapi/session/hello",
      dataStream.str(), true);

  Document doc;
  doc.Parse(jsonString.c_str());
  if (!doc.GetParseError() && doc["success"].GetBool())
  {
    XBMC->Log(LOG_DEBUG, "Hello was successful.");
    return true;
  }
  else
  {
    XBMC->Log(LOG_NOTICE, "Hello failed.");
    return false;
  }
}

Document ZatData::Login()
{
  XBMC->Log(LOG_DEBUG, "Try to login.");

  ostringstream dataStream;
  dataStream << "login=" << Utils::UrlEncode(username) << "&password="
      << Utils::UrlEncode(password) << "&format=json&remember=true";
  string jsonString = HttpPost(providerUrl + "/zapi/v2/account/login",
      dataStream.str(), true, user_agent);

  Document doc;
  doc.Parse(jsonString.c_str());
  return doc;
}

bool ZatData::InitSession()
{
  string jsonString = HttpGet(providerUrl + "/zapi/v2/session", true);
  Document doc;
  doc.Parse(jsonString.c_str());
  if (doc.GetParseError() || !doc["success"].GetBool())
  {
    XBMC->Log(LOG_ERROR, "Initialize session failed.");
    return false;
  }

  if (!doc["session"]["loggedin"].GetBool())
  {
    XBMC->Log(LOG_DEBUG, "Need to login.");
    pzuid = "";
    beakerSessionId = "";
    WriteDataJson();
    doc = Login(); 
    if (doc.GetParseError() || !doc["success"].GetBool()
        || !doc["session"]["loggedin"].GetBool())
    {
      XBMC->Log(LOG_ERROR, "Login failed.");
      return false;
    }
    else
    {
        XBMC->Log(LOG_DEBUG, "Login was successful.");
    }
  }

  const Value& session = doc["session"];

  countryCode = GetStringOrEmpty(session, "aliased_country_code");
  serviceRegionCountry = GetStringOrEmpty(session, "service_region_country");
  recallEnabled = session["recall_eligible"].GetBool();
  selectiveRecallEnabled = session.HasMember("selective_recall_eligible") ? session["selective_recall_eligible"].GetBool() : false;
  recordingEnabled = session["recording_eligible"].GetBool();
  XBMC->Log(LOG_NOTICE, "Country code: %s", countryCode.c_str());
  XBMC->Log(LOG_NOTICE, "Service region country: %s", serviceRegionCountry.c_str());
  XBMC->Log(LOG_NOTICE, "Stream type: %s", streamType.c_str());
  if (recallEnabled)
  {
    maxRecallSeconds = session["recall_seconds"].GetInt();
    XBMC->Log(LOG_NOTICE, "Recall is enabled for %d seconds.", maxRecallSeconds);
  }
  else
  {
    XBMC->Log(LOG_NOTICE, "Recall is disabled");
  }
  XBMC->Log(LOG_NOTICE, "Selective recall is %s", selectiveRecallEnabled ? "enabled" : "disabled");
  XBMC->Log(LOG_NOTICE, "Recordings are %s", recordingEnabled ? "enabled" : "disabled");
  powerHash = GetStringOrEmpty(session, "power_guide_hash");
  return true;
}

bool ZatData::LoadChannels()
{
  map<string, ZatChannel> allChannels;
  string jsonString = HttpGet(providerUrl + "/zapi/channels/favorites");
  Document favDoc;
  favDoc.Parse(jsonString.c_str());

  if (favDoc.GetParseError() || !favDoc["success"].GetBool())
  {
    return false;
  }
  const Value& favs = favDoc["favorites"];

  ostringstream urlStream;
  urlStream << providerUrl + "/zapi/v2/cached/channels/" << powerHash
      << "?details=False";
  jsonString = HttpGet(urlStream.str());

  Document doc;
  doc.Parse(jsonString.c_str());
  if (doc.GetParseError() || !doc["success"].GetBool())
  {
    XBMC->Log(LOG_ERROR,"Failed to load channels");
    return false;
  }

  int channelNumber = favs.Size();
  const Value& groups = doc["channel_groups"];

  //Load the channel groups and channels
  for (Value::ConstValueIterator itr = groups.Begin(); itr != groups.End();
      ++itr)
  {
    PVRZattooChannelGroup group;
    const Value& groupItem = (*itr);
    group.name = GetStringOrEmpty(groupItem, "name");
    const Value& channels = groupItem["channels"];
    for (Value::ConstValueIterator itr1 = channels.Begin();
        itr1 != channels.End(); ++itr1)
    {
      const Value& channelItem = (*itr1);
      const Value& qualities = channelItem["qualities"];
      for (Value::ConstValueIterator itr2 = qualities.Begin();
          itr2 != qualities.End(); ++itr2)
      {
        const Value& qualityItem = (*itr2);
        string avail = GetStringOrEmpty(qualityItem, "availability");
        if (avail == "available")
        {
          ZatChannel channel;
          channel.name = GetStringOrEmpty(qualityItem, "title");
          string cid = GetStringOrEmpty(channelItem, "cid");
          channel.iUniqueId = GetChannelId(cid.c_str());
          channel.cid = cid;
          channel.iChannelNumber = ++channelNumber;
          channel.strLogoPath = "http://logos.zattic.com";
          channel.strLogoPath.append(GetStringOrEmpty(qualityItem, "logo_white_84"));
          channel.selectiveRecallSeconds = channelItem.HasMember("selective_recall_seconds") ? channelItem["selective_recall_seconds"].GetInt() : 0;
          channel.recordingEnabled = channelItem.HasMember("recording") ? channelItem["recording"].GetBool() : false;
          group.channels.insert(group.channels.end(), channel);
          allChannels[cid] = channel;
          channelsByCid[channel.cid] = channel;
          channelsByUid[channel.iUniqueId] = channel;
          break;
        }
      }
    }
    if (!favoritesOnly && !group.channels.empty())
      channelGroups.insert(channelGroups.end(), group);
  }

  PVRZattooChannelGroup favGroup;
  favGroup.name = "Favoriten";

  for (Value::ConstValueIterator itr = favs.Begin(); itr != favs.End(); ++itr)
  {
    const Value& favItem = (*itr);
    string favCid = favItem.GetString();
    if (allChannels.find(favCid) != allChannels.end())
    {
      ZatChannel channel = allChannels[favCid];
      channel.iChannelNumber = static_cast<int>(favGroup.channels.size() + 1);
      favGroup.channels.insert(favGroup.channels.end(), channel);
      channelsByCid[channel.cid] = channel;
      channelsByUid[channel.iUniqueId] = channel;
    }
  }

  if (!favGroup.channels.empty())
    channelGroups.insert(channelGroups.end(), favGroup);

  return true;
}

int ZatData::GetChannelId(const char * strChannelName)
{
  int iId = 0;
  int c;
  while ((c = *strChannelName++))
    iId = ((iId << 5) + iId) + c; /* iId * 33 + c */
  return abs(iId);
}

int ZatData::GetChannelGroupsAmount()
{
  return static_cast<int>(channelGroups.size());
}

ZatData::ZatData(const string& u, const string& p, bool favoritesOnly,
    bool alternativeEpgService, const string& streamType, int provider,
    const string& xmlTVFile) :
    recordingEnabled(false), uuid(""), xmlTV(nullptr)
{
  XBMC->Log(LOG_NOTICE, "Using useragent: %s", user_agent.c_str());
  username = u;
  password = p;
  this->alternativeEpgService = alternativeEpgService;
  this->favoritesOnly = favoritesOnly;
  this->streamType = streamType;
  this->xmlTVFile = xmlTVFile;
  for (int i = 0; i < 5; ++i)
  {
    updateThreads.emplace_back(new UpdateThread(i, this));
  }
  
  switch (provider) {
    case 1:
      providerUrl = "https://www.netplus.tv";
      break;
    case 2:
      providerUrl = "https://mobiltv.quickline.com";
      break;
    case 3:
      providerUrl = "https://tvplus.m-net.de";
      break;
    case 4:
      providerUrl = "https://player.waly.tv";
      break;
    case 5:
      providerUrl = "https://www.meinewelt.cc";
      break;
    case 6:
      providerUrl = "https://www.bbv-tv.net";
      break;
    case 7:
      providerUrl = "https://www.vtxtv.ch";
      break;
    case 8:
      providerUrl = "https://www.myvisiontv.ch";
      break;
    case 9:
      providerUrl = "https://iptv.glattvision.ch";
      break;
    case 10:
      providerUrl = "https://www.saktv.ch";
      break;
    case 11:
      providerUrl = "https://nettv.netcologne.de";
      break;
    case 12:
      providerUrl = "https://tvonline.ewe.de";
      break;
    case 13:
      providerUrl = "https://www.quantum-tv.com";
      break;
    case 14:
      providerUrl = "https://tv.salt.ch";
      break;
    default:
      providerUrl = "https://zattoo.com";
  }
  
  ReadDataJson();
  if (!xmlTVFile.empty()) {
    xmlTV = new XmlTV(xmlTVFile);
  }
}

ZatData::~ZatData()
{
  for (auto const &updateThread : updateThreads)
  {
    updateThread->StopThread(200);
    delete updateThread;
  }
  for (auto const& item : recordingsData)
  {
    delete item.second;
  }
  channelGroups.clear();
  if (xmlTV) {
    delete xmlTV;
  }
}

bool ZatData::Initialize()
{
  if (!LoadAppId()) {
      return false;
  }
  
  string uuid = GetUUID();
  
  SendHello(uuid);
  //Ignore if hello fails

  return InitSession();
}

void ZatData::GetAddonCapabilities(PVR_ADDON_CAPABILITIES* pCapabilities)
{
  pCapabilities->bSupportsRecordings = recordingEnabled;
  pCapabilities->bSupportsTimers = recordingEnabled;
}

PVR_ERROR ZatData::GetChannelGroups(ADDON_HANDLE handle)
{
  vector<PVRZattooChannelGroup>::iterator it;
  for (it = channelGroups.begin(); it != channelGroups.end(); ++it)
  {
    PVR_CHANNEL_GROUP xbmcGroup;
    memset(&xbmcGroup, 0, sizeof(PVR_CHANNEL_GROUP));
    xbmcGroup.iPosition = 0; /* not supported  */
    xbmcGroup.bIsRadio = false; /* is radio group */
    strncpy(xbmcGroup.strGroupName, it->name.c_str(),
        sizeof(xbmcGroup.strGroupName) - 1);

    PVR->TransferChannelGroup(handle, &xbmcGroup);
  }
  return PVR_ERROR_NO_ERROR;
}

PVRZattooChannelGroup *ZatData::FindGroup(const string &strName)
{
  vector<PVRZattooChannelGroup>::iterator it;
  for (it = channelGroups.begin(); it < channelGroups.end(); ++it)
  {
    if (it->name == strName)
      return &*it;
  }

  return nullptr;
}

PVR_ERROR ZatData::GetChannelGroupMembers(ADDON_HANDLE handle,
    const PVR_CHANNEL_GROUP &group)
{
  PVRZattooChannelGroup *myGroup;
  if ((myGroup = FindGroup(group.strGroupName)) != nullptr)
  {
    vector<ZatChannel>::iterator it;
    for (it = myGroup->channels.begin(); it != myGroup->channels.end(); ++it)
    {
      ZatChannel &channel = (*it);
      PVR_CHANNEL_GROUP_MEMBER xbmcGroupMember;
      memset(&xbmcGroupMember, 0, sizeof(PVR_CHANNEL_GROUP_MEMBER));

      strncpy(xbmcGroupMember.strGroupName, group.strGroupName,
          sizeof(xbmcGroupMember.strGroupName) - 1);
      xbmcGroupMember.iChannelUniqueId = static_cast<unsigned int>(channel.iUniqueId);
      xbmcGroupMember.iChannelNumber = static_cast<unsigned int>(channel.iChannelNumber);

      PVR->TransferChannelGroupMember(handle, &xbmcGroupMember);
    }
  }

  return PVR_ERROR_NO_ERROR;
}

int ZatData::GetChannelsAmount()
{
  return static_cast<int>(channelsByCid.size());
}

PVR_ERROR ZatData::GetChannels(ADDON_HANDLE handle, bool bRadio)
{
  vector<PVRZattooChannelGroup>::iterator it;
  for (it = channelGroups.begin(); it != channelGroups.end(); ++it)
  {
    vector<ZatChannel>::iterator it2;
    for (it2 = it->channels.begin(); it2 != it->channels.end(); ++it2)
    {
      ZatChannel &channel = (*it2);

      PVR_CHANNEL kodiChannel;
      memset(&kodiChannel, 0, sizeof(PVR_CHANNEL));

      kodiChannel.iUniqueId = static_cast<unsigned int>(channel.iUniqueId);
      kodiChannel.bIsRadio = false;
      kodiChannel.iChannelNumber = static_cast<unsigned int>(channel.iChannelNumber);
      strncpy(kodiChannel.strChannelName, channel.name.c_str(),
          sizeof(kodiChannel.strChannelName) - 1);
      kodiChannel.iEncryptionSystem = 0;

      ostringstream iconStream;
      iconStream
          << "special://home/addons/pvr.zattoo/resources/media/channel_logo/"
          << channel.cid << ".png";
      string iconPath = iconStream.str();
      if (!XBMC->FileExists(iconPath.c_str(), true))
      {
        ostringstream iconStreamSystem;
        iconStreamSystem
            << "special://xbmc/addons/pvr.zattoo/resources/media/channel_logo/"
            << channel.cid << ".png";
        iconPath = iconStreamSystem.str();
        if (!XBMC->FileExists(iconPath.c_str(), true))
        {
          XBMC->Log(LOG_INFO,
              "No logo found for channel '%s'. Fallback to Zattoo-Logo.",
              channel.cid.c_str());
          iconPath = channel.strLogoPath;
        }
      }

      strncpy(kodiChannel.strIconPath, iconPath.c_str(),
          sizeof(kodiChannel.strIconPath) - 1);

      kodiChannel.bIsHidden = false;

      PVR->TransferChannelEntry(handle, &kodiChannel);

    }
  }
  return PVR_ERROR_NO_ERROR;
}

string ZatData::GetChannelStreamUrl(int uniqueId)
{

  ZatChannel *channel = FindChannel(uniqueId);
  XBMC->Log(LOG_DEBUG, "Get live url for channel %s", channel->cid.c_str());

  ostringstream dataStream;
  dataStream << "cid=" << channel->cid << "&stream_type=" << streamType
      << "&format=json";

  if (recallEnabled)
  {
    dataStream << "&timeshift=" << maxRecallSeconds;
  }

  string jsonString = HttpPost(providerUrl + "/zapi/watch", dataStream.str());

  Document doc;
  doc.Parse(jsonString.c_str());
  if (doc.GetParseError() || !doc["success"].GetBool())
  {
    return "";
  }
  string url = GetStringOrEmpty(doc["stream"], "url");
  XBMC->Log(LOG_DEBUG, "Got url: %s", url.c_str());
  return url;

}

ZatChannel *ZatData::FindChannel(int uniqueId)
{
  vector<PVRZattooChannelGroup>::iterator it;
  for (it = channelGroups.begin(); it != channelGroups.end(); ++it)
  {
    vector<ZatChannel>::iterator it2;
    for (it2 = it->channels.begin(); it2 != it->channels.end(); ++it2)
    {
      ZatChannel &channel = (*it2);
      if (channel.iUniqueId == uniqueId)
      {
        return &channel;
      }
    }
  }
  return nullptr;
}

void ZatData::GetEPGForChannelExternalService(int uniqueChannelId,
    time_t iStart, time_t iEnd)
{
  ZatChannel *zatChannel = FindChannel(uniqueChannelId);
  string cid = zatChannel->cid;
  ostringstream urlStream;
  urlStream << "http://zattoo.buehlmann.net/epg/api/Epg/" << serviceRegionCountry << "/"
      << powerHash << "/" << cid << "/" << iStart << "/" << iEnd;
  string jsonString = HttpGetCached(urlStream.str(), 3600, user_agent);
  Document doc;
  doc.Parse(jsonString.c_str());
  if (doc.GetParseError())
  {
    return;
  }
  for (Value::ConstValueIterator itr = doc.Begin(); itr != doc.End(); ++itr)
  {
    const Value& program = (*itr);
    EPG_TAG tag;
    memset(&tag, 0, sizeof(EPG_TAG));

    tag.iUniqueBroadcastId = static_cast<unsigned int>(program["Id"].GetInt());
    string title = GetStringOrEmpty(program, "Title");
    tag.strTitle = title.c_str();
    tag.iUniqueChannelId = static_cast<unsigned int>(zatChannel->iUniqueId);
    tag.startTime = Utils::StringToTime(GetStringOrEmpty(program, "StartTime"));
    tag.endTime = Utils::StringToTime(GetStringOrEmpty(program, "EndTime"));
    string description = GetStringOrEmpty(program, "Description");
    tag.strPlotOutline = description.c_str();
    tag.strPlot = description.c_str();
    tag.strOriginalTitle = nullptr; /* not supported */
    tag.strCast = nullptr; /* not supported */
    tag.strDirector = nullptr; /*SA not supported */
    tag.strWriter = nullptr; /* not supported */
    tag.iYear = 0; /* not supported */
    tag.strIMDBNumber = nullptr; /* not supported */
    tag.strIconPath = GetStringOrEmpty(program, "ImageUrl").c_str();
    tag.iParentalRating = 0; /* not supported */
    tag.iStarRating = 0; /* not supported */
    tag.bNotify = false; /* not supported */
    tag.iSeriesNumber = 0; /* not supported */
    tag.iEpisodeNumber = 0; /* not supported */
    tag.iEpisodePartNumber = 0; /* not supported */
    tag.strEpisodeName = GetStringOrEmpty(program, "Subtitle").c_str();
    tag.iFlags = EPG_TAG_FLAG_UNDEFINED;
    string genreStr = GetStringOrEmpty(program, "Genre");
    int genre = categories.Category(genreStr);
    if (genre)
    {
      tag.iGenreSubType = genre & 0x0F;
      tag.iGenreType = genre & 0xF0;
    }
    else
    {
      tag.iGenreType = EPG_GENRE_USE_STRING;
      tag.iGenreSubType = 0; /* not supported */
      tag.strGenreDescription = genreStr.c_str();
    }

    PVR->EpgEventStateChange(&tag, EPG_EVENT_CREATED);
  }

}

void ZatData::GetEPGForChannel(const PVR_CHANNEL &channel, time_t iStart,
    time_t iEnd)
{
  UpdateThread::LoadEpg(channel.iUniqueId, iStart, iEnd);
}

void ZatData::GetEPGForChannelAsync(int uniqueChannelId, time_t iStart,
    time_t iEnd)
{
  ZatChannel *zatChannel = FindChannel(uniqueChannelId);
  
  if (xmlTV && xmlTV->GetEPGForChannel(zatChannel->cid, uniqueChannelId)) {
    return;
  }
  
  if (this->alternativeEpgService)
  {
    GetEPGForChannelExternalService(uniqueChannelId, iStart, iEnd);
    return;
  }

  map<time_t, PVRIptvEpgEntry>* channelEpgCache = LoadEPG(iStart, iEnd, uniqueChannelId);
  if (channelEpgCache == nullptr)
  {
    XBMC->Log(LOG_NOTICE,
        "Loading epg faild for channel '%s' from %lu to %lu",
        zatChannel->name.c_str(), iStart, iEnd);
    return;
  }
  for (auto const &entry : *channelEpgCache)
  {
    PVRIptvEpgEntry epgEntry = entry.second;

    EPG_TAG tag;
    memset(&tag, 0, sizeof(EPG_TAG));

    tag.iUniqueBroadcastId = static_cast<unsigned int>(epgEntry.iBroadcastId);
    tag.strTitle = epgEntry.strTitle.c_str();
    tag.iUniqueChannelId = static_cast<unsigned int>(epgEntry.iChannelId);
    tag.startTime = epgEntry.startTime;
    tag.endTime = epgEntry.endTime;
    tag.strPlotOutline = epgEntry.strPlot.c_str(); //epgEntry.strPlotOutline.c_str();
    tag.strPlot = epgEntry.strPlot.c_str();
    tag.strOriginalTitle = nullptr; /* not supported */
    tag.strCast = nullptr; /* not supported */
    tag.strDirector = nullptr; /*SA not supported */
    tag.strWriter = nullptr; /* not supported */
    tag.iYear = 0; /* not supported */
    tag.strIMDBNumber = nullptr; /* not supported */
    tag.strIconPath = epgEntry.strIconPath.c_str();
    tag.iParentalRating = 0; /* not supported */
    tag.iStarRating = 0; /* not supported */
    tag.bNotify = false; /* not supported */
    tag.iSeriesNumber = 0; /* not supported */
    tag.iEpisodeNumber = 0; /* not supported */
    tag.iEpisodePartNumber = 0; /* not supported */
    tag.strEpisodeName = nullptr; /* not supported */
    tag.iFlags = EPG_TAG_FLAG_UNDEFINED;

    int genre = categories.Category(epgEntry.strGenreString);
    if (genre)
    {
      tag.iGenreSubType = genre & 0x0F;
      tag.iGenreType = genre & 0xF0;
    }
    else
    {
      tag.iGenreType = EPG_GENRE_USE_STRING;
      tag.iGenreSubType = 0; /* not supported */
      tag.strGenreDescription = epgEntry.strGenreString.c_str();
    }

    PVR->EpgEventStateChange(&tag, EPG_EVENT_CREATED);
  }
  delete channelEpgCache;
}

map<time_t, PVRIptvEpgEntry>* ZatData::LoadEPG(time_t iStart, time_t iEnd, int uniqueChannelId)
{
  //Do some time magic that the start date is not to far in the past because zattoo doesnt like that
  time_t tempStart = iStart - (iStart % (3600 / 2)) - 86400;
  time_t tempEnd = tempStart + 3600 * 5; //Add 5 hours

  auto *epgCache = new map<time_t, PVRIptvEpgEntry>();
  
  while (tempEnd <= iEnd)
  {
    ostringstream urlStream;
    urlStream << providerUrl << "/zapi/v2/cached/program/power_guide/"
        << powerHash << "?end=" << tempEnd << "&start=" << tempStart
        << "&format=json";

    string jsonString = HttpGetCached(urlStream.str(), 3600);

    Document doc;
    doc.Parse(jsonString.c_str());
    if (doc.GetParseError() || !doc["success"].GetBool())
    {
      return nullptr;
    }

    const Value& channels = doc["channels"];
    
    //Load the channel groups and channels
    for (Value::ConstValueIterator itr = channels.Begin();
        itr != channels.End(); ++itr)
    {
      const Value& channelItem = (*itr);
      string cid = GetStringOrEmpty(channelItem, "cid");

      int channelId = GetChannelId(cid.c_str());
      ZatChannel *channel = FindChannel(channelId);

      if (!channel || channel->iUniqueId != uniqueChannelId)
      {
        continue;
      }

      const Value& programs = channelItem["programs"];
      for (Value::ConstValueIterator itr1 = programs.Begin();
          itr1 != programs.End(); ++itr1)
      {
        const Value& program = (*itr1);
        
        const Type& checkType = program["t"].GetType();
        if( checkType != kStringType )
          continue;

        PVRIptvEpgEntry entry;
        entry.strTitle = GetStringOrEmpty(program, "t");
        entry.startTime = program["s"].GetInt();
        entry.endTime = program["e"].GetInt();
        entry.iBroadcastId = program["id"].GetInt();
        entry.strIconPath = GetStringOrEmpty(program, "i_url");
        entry.iChannelId = channel->iUniqueId;
        entry.strPlot = GetStringOrEmpty(program, "et");

        const Value& genres = program["g"];
        for (Value::ConstValueIterator itr2 = genres.Begin();
            itr2 != genres.End(); ++itr2)
        {
          entry.strGenreString = (*itr2).GetString();
          break;
        }

        (*epgCache)[entry.startTime] = entry;
      }
    }
    tempStart = tempEnd;
    tempEnd = tempStart + 3600 * 5; //Add 5 hours
  }
  return epgCache;
}

void ZatData::SetRecordingPlayCount(const PVR_RECORDING &recording, int count)
{
  string recordingId = recording.strRecordingId;
  ZatRecordingData *recData;
  if (recordingsData.find(recordingId) != recordingsData.end())
  {
    recData = recordingsData[recordingId];
    recData->playCount = count;
  }
  else
  {
    recData = new ZatRecordingData();
    recData->playCount = count;
    recData->recordingId = recordingId;
    recData->lastPlayedPosition = 0;
    recData->stillValid = true;
    recordingsData[recordingId] = recData;
  }

  WriteDataJson();
}

void ZatData::SetRecordingLastPlayedPosition(const PVR_RECORDING &recording,
    int lastplayedposition)
{
  string recordingId = recording.strRecordingId;
  ZatRecordingData *recData;
  if (recordingsData.find(recordingId) != recordingsData.end())
  {
    recData = recordingsData[recordingId];
    recData->lastPlayedPosition = lastplayedposition;
  }
  else
  {
    recData = new ZatRecordingData();
    recData->playCount = 0;
    recData->recordingId = recordingId;
    recData->lastPlayedPosition = lastplayedposition;
    recData->stillValid = true;
    recordingsData[recordingId] = recData;
  }

  WriteDataJson();
}

int ZatData::GetRecordingLastPlayedPosition(const PVR_RECORDING &recording)
{
  if (recordingsData.find(recording.strRecordingId) != recordingsData.end())
  {
    ZatRecordingData* recData = recordingsData[recording.strRecordingId];
    return recData->lastPlayedPosition;
  }
  return 0;
}

void ZatData::GetRecordings(ADDON_HANDLE handle, bool future)
{
  string jsonString = HttpGetCached(providerUrl + "/zapi/playlist", 60);

  Document doc;
  doc.Parse(jsonString.c_str());
  if (doc.GetParseError() || !doc["success"].GetBool())
  {
    return;
  }

  const Value& recordings = doc["recordings"];

  time_t current_time;
  time(&current_time);
  
  string idList;
  
  map<int, ZatRecordingDetails> detailsById;
  Value::ConstValueIterator recordingsItr = recordings.Begin();
  while (recordingsItr != recordings.End()) {
    int bucketSize = 100;
    ostringstream urlStream;
    urlStream << providerUrl << "/zapi/v2/cached/program/power_details/" << powerHash  << "?complete=True&program_ids=";
    while (bucketSize > 0 && recordingsItr != recordings.End()) {
      const Value& recording = (*recordingsItr);
      if (bucketSize < 100) {
        urlStream << ",";
      }
      urlStream << recording["program_id"].GetInt();
      ++recordingsItr;
      bucketSize--;
    }
    
    jsonString = HttpGetCached(urlStream.str(), 60 * 60 * 24 * 30);
    Document detailDoc;
    detailDoc.Parse(jsonString.c_str());
    if (detailDoc.GetParseError() || !detailDoc["success"].GetBool())
    {
      XBMC->Log(LOG_ERROR, "Failed to load details for recordings.");
    }
    else
    {
      const Value& programs = detailDoc["programs"];
      for (Value::ConstValueIterator progItr = programs.Begin();
          progItr != programs.End(); ++progItr)
      {
        const Value &program = *progItr;
        ZatRecordingDetails details;
        if (program.HasMember("g") && program["g"].IsArray() && program["g"].Begin() != program["g"].End()) {
          details.genre = program["g"].Begin()->GetString();
        } else {
          details.genre = "";
        }
        details.description = GetStringOrEmpty(program, "d");
        detailsById.insert(pair<int, ZatRecordingDetails>(program["id"].GetInt(), details));
      }
    }

  }
    
  for (Value::ConstValueIterator itr = recordings.Begin();
      itr != recordings.End(); ++itr)
  {
    const Value& recording = (*itr);
    int programId = recording["program_id"].GetInt();

    string cid = GetStringOrEmpty(recording, "cid");
    auto iterator = channelsByCid.find(cid);
    if (iterator == channelsByCid.end()) {
      XBMC->Log(LOG_ERROR, "Channel %s not found for recording: %i", cid.c_str(), programId);
      continue;
    }
    ZatChannel channel = iterator->second;

    auto detailIterator = detailsById.find(programId);
    bool hasDetails = detailIterator != detailsById.end();
    
    //genre
    int genre = 0;
    if (hasDetails) {
      genre = categories.Category(detailIterator->second.genre);
    }

    time_t startTime = Utils::StringToTime(GetStringOrEmpty(recording, "start"));
    if (future && (startTime > current_time))
    {
      PVR_TIMER tag;
      memset(&tag, 0, sizeof(PVR_TIMER));

      tag.iClientIndex = static_cast<unsigned int>(recording["id"].GetInt());
      PVR_STRCPY(tag.strTitle, GetStringOrEmpty(recording, "title").c_str());
      PVR_STRCPY(tag.strSummary, GetStringOrEmpty(recording, "episode_title").c_str());
      time_t endTime = Utils::StringToTime(GetStringOrEmpty(recording, "end").c_str());
      tag.startTime = startTime;
      tag.endTime = endTime;
      tag.state = PVR_TIMER_STATE_SCHEDULED;
      tag.iTimerType = 1;
      tag.iEpgUid = static_cast<unsigned int>(recording["program_id"].GetInt());
      tag.iClientChannelUid = channel.iUniqueId;
      PVR->TransferTimerEntry(handle, &tag);
      UpdateThread::SetNextRecordingUpdate(startTime);
      if (genre)
      {
        tag.iGenreSubType = genre & 0x0F;
        tag.iGenreType = genre & 0xF0;
      }

    }
    else if (!future && (startTime <= current_time))
    {
      PVR_RECORDING tag;
      memset(&tag, 0, sizeof(PVR_RECORDING));
      tag.bIsDeleted = false;

      PVR_STRCPY(tag.strRecordingId,
          to_string(recording["id"].GetInt()).c_str());
      PVR_STRCPY(tag.strTitle, GetStringOrEmpty(recording, "title").c_str());
      PVR_STRCPY(tag.strEpisodeName, GetStringOrEmpty(recording, "episode_title").c_str());
      PVR_STRCPY(tag.strPlot,
          hasDetails ? detailIterator->second.description.c_str() : "");
      PVR_STRCPY(tag.strIconPath, GetStringOrEmpty(recording, "image_url").c_str());
      tag.iChannelUid = channel.iUniqueId;
      PVR_STRCPY(tag.strChannelName, channel.name.c_str());
      time_t endTime = Utils::StringToTime(GetStringOrEmpty(recording, "end").c_str());
      tag.recordingTime = startTime;
      tag.iDuration = static_cast<int>(endTime - startTime);

      if (genre)
      {
        tag.iGenreSubType = genre & 0x0F;
        tag.iGenreType = genre & 0xF0;
      }

      if (recordingsData.find(tag.strRecordingId) != recordingsData.end())
      {
        ZatRecordingData* recData = recordingsData[tag.strRecordingId];
        tag.iPlayCount = recData->playCount;
        tag.iLastPlayedPosition = recData->lastPlayedPosition;
        recData->stillValid = true;
      }

      PVR->TransferRecordingEntry(handle, &tag);
      recordingsLoaded = true;
    }
  }
}

int ZatData::GetRecordingsAmount(bool future)
{
  string jsonString = HttpGetCached(providerUrl + "/zapi/playlist", 60);

  time_t current_time;
  time(&current_time);

  Document doc;
  doc.Parse(jsonString.c_str());
  if (doc.GetParseError() || !doc["success"].GetBool())
  {
    return 0;
  }

  const Value& recordings = doc["recordings"];

  int count = 0;
  for (Value::ConstValueIterator itr = recordings.Begin();
      itr != recordings.End(); ++itr)
  {
    const Value& recording = (*itr);
    time_t startTime = Utils::StringToTime(GetStringOrEmpty(recording, "start"));
    if (future == (startTime > current_time))
    {
      count++;
    }
  }
  return count;
}

string ZatData::GetRecordingStreamUrl(const string& recordingId)
{
  XBMC->Log(LOG_DEBUG, "Get url for recording %s", recordingId.c_str());
  
  ostringstream dataStream;
  dataStream << "recording_id=" << recordingId << "&stream_type=" << streamType;

  string jsonString = HttpPost(providerUrl + "/zapi/watch", dataStream.str());

  Document doc;
  doc.Parse(jsonString.c_str());
  if (doc.GetParseError() || !doc["success"].GetBool())
  {
    return "";
  }

  string url = GetStringOrEmpty(doc["stream"], "url");
  XBMC->Log(LOG_DEBUG, "Got url: %s", url.c_str());
  return url;

}

bool ZatData::Record(int programId)
{
  ostringstream dataStream;
  dataStream << "program_id=" << programId;

  string jsonString = HttpPost(providerUrl + "/zapi/playlist/program",
      dataStream.str());
  Document doc;
  doc.Parse(jsonString.c_str());
  return !doc.GetParseError() && doc["success"].GetBool();
}

bool ZatData::DeleteRecording(const string& recordingId)
{
  ostringstream dataStream;
  dataStream << "recording_id=" << recordingId << "";

  string jsonString = HttpPost(providerUrl + "/zapi/playlist/remove",
      dataStream.str());

  Document doc;
  doc.Parse(jsonString.c_str());
  return !doc.GetParseError() && doc["success"].GetBool();
}

bool ZatData::IsPlayable(const EPG_TAG *tag)
{
  time_t current_time;
  time(&current_time);
  if (tag->startTime > current_time) {
    return false;
  }
  int recallSeconds = GetRecallSeconds(tag);
  if (recallSeconds == 0)
  {
    return false;
  }
  if (current_time < tag->endTime)
  {
    return true;    
  }
  return (current_time - tag->endTime) < recallSeconds;
}

int ZatData::GetRecallSeconds(const EPG_TAG *tag) {
  if (recallEnabled)
  {
    return static_cast<int>(maxRecallSeconds);
  }
  if (selectiveRecallEnabled)
  {
    ZatChannel channel = channelsByUid[tag->iUniqueChannelId];
    return channel.selectiveRecallSeconds;
  }
  return 0;
}

bool ZatData::IsRecordable(const EPG_TAG *tag)
{
  if (!recordingEnabled)
  {
    return false;
  }
  ZatChannel channel = channelsByUid[tag->iUniqueChannelId];
  if (!channel.recordingEnabled) {
    return false;
  }
  int recallSeconds = GetRecallSeconds(tag);
  time_t current_time;
  time(&current_time);
  if (recallSeconds == 0)
  {
    return current_time < tag->startTime;
  }
  return ((current_time - tag->endTime) < recallSeconds);
}

string ZatData::GetEpgTagUrl(const EPG_TAG *tag)
{
  ostringstream dataStream;
  ZatChannel channel = channelsByUid[tag->iUniqueChannelId];
  char timeStart[sizeof "2011-10-08T07:07:09Z"];
  struct tm tm{};
  gmtime_r(&tag->startTime, &tm);
  strftime(timeStart, sizeof timeStart, "%FT%TZ", &tm);
  char timeEnd[sizeof "2011-10-08T07:07:09Z"];
  gmtime_r(&tag->endTime, &tm);
  strftime(timeEnd, sizeof timeEnd, "%FT%TZ", &tm);
  
  string jsonString;
  
  XBMC->Log(LOG_DEBUG, "Get timeshift url for channel %s at %s", channel.cid.c_str(), timeStart);
  
  if (recallEnabled)
  {
    dataStream << "cid=" << channel.cid << "&start=" << timeStart << "&end="
      << timeEnd << "&stream_type=" << streamType;
    jsonString = HttpPost(providerUrl + "/zapi/watch", dataStream.str());
  }
  else if (selectiveRecallEnabled)
  {
    dataStream << "https_watch_urls=True" << "&stream_type=" << streamType;
    jsonString = HttpPost(providerUrl + "/zapi/watch/selective_recall/" + channel.cid + "/" + to_string(tag->iUniqueBroadcastId), dataStream.str());  
  }
  else
  {
    return "";
  }

  Document doc;
  doc.Parse(jsonString.c_str());
  if (doc.GetParseError() || !doc["success"].GetBool())
  {
    return "";
  }
  string url = GetStringOrEmpty(doc["stream"], "url");
  XBMC->Log(LOG_DEBUG, "Got url: %s", url.c_str());
  return url;
}

string ZatData::GetStringOrEmpty(const Value& jsonValue, const char* fieldName)
{
  if (!jsonValue.HasMember(fieldName) || !jsonValue[fieldName].IsString())
  {
    return "";
  }
  return jsonValue[fieldName].GetString();
}
