/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "ScraperUrl.h"

#include "CharsetConverter.h"
#include "ServiceBroker.h"
#include "URIUtils.h"
#include "URL.h"
#include "XMLUtils.h"
#include "filesystem/CurlFile.h"
#include "filesystem/ZipFile.h"
#include "settings/AdvancedSettings.h"
#include "settings/SettingsComponent.h"
#include "utils/CharsetDetection.h"
#include "utils/Mime.h"
#include "utils/StringUtils.h"
#include "utils/XBMCTinyXML.h"
#include "utils/log.h"

#include <algorithm>
#include <cstring>
#include <sstream>

CScraperUrl::CScraperUrl() : m_relevance(0.0)
{
}

CScraperUrl::CScraperUrl(std::string strUrl) : CScraperUrl()
{
  ParseString(std::move(strUrl));
}

CScraperUrl::CScraperUrl(const TiXmlElement* element) : CScraperUrl()
{
  ParseElement(element);
}

CScraperUrl::~CScraperUrl() = default;

void CScraperUrl::Clear()
{
  m_url.clear();
  m_data.clear();
  m_relevance = 0.0;
}

bool CScraperUrl::Parse()
{
  auto strToParse = m_data;
  m_data.clear();
  return ParseString(std::move(strToParse));
}

bool CScraperUrl::ParseElement(const TiXmlElement* element)
{
  if (!element || !element->FirstChild() || !element->FirstChild()->Value())
    return false;

  std::stringstream stream;
  stream << *element;
  m_data += stream.str();

  SUrlEntry url;
  url.m_url = element->FirstChild()->Value();
  url.m_spoof = XMLUtils::GetAttribute(element, "spoof");
  const char* szPost = element->Attribute("post");
  if (szPost && StringUtils::CompareNoCase(szPost, "yes") == 0)
    url.m_post = true;
  else
    url.m_post = false;
  const char* szIsGz = element->Attribute("gzip");
  if (szIsGz && StringUtils::CompareNoCase(szIsGz, "yes") == 0)
    url.m_isgz = true;
  else
    url.m_isgz = false;
  url.m_cache = XMLUtils::GetAttribute(element, "cache");

  const char* szType = element->Attribute("type");
  url.m_type = UrlType::General;
  url.m_season = -1;
  if (szType && StringUtils::CompareNoCase(szType, "season") == 0)
  {
    url.m_type = UrlType::Season;
    const char* szSeason = element->Attribute("season");
    if (szSeason)
      url.m_season = atoi(szSeason);
  }
  url.m_aspect = XMLUtils::GetAttribute(element, "aspect");

  m_url.push_back(url);

  return true;
}

bool CScraperUrl::ParseString(std::string strUrl)
{
  if (strUrl.empty())
    return false;

  CXBMCTinyXML doc;
  /* strUrl is coming from internal sources (usually generated by scraper or from database)
   * so strUrl is always in UTF-8 */
  doc.Parse(strUrl, TIXML_ENCODING_UTF8);

  auto pElement = doc.RootElement();
  if (pElement == nullptr)
  {
    SUrlEntry url;
    url.m_url = strUrl;
    url.m_type = UrlType::General;
    url.m_season = -1;
    url.m_post = false;
    url.m_isgz = false;
    m_url.push_back(url);
    m_data = strUrl;
  }
  else
  {
    while (pElement != nullptr)
    {
      ParseElement(pElement);
      pElement = pElement->NextSiblingElement(pElement->Value());
    }
  }

  return true;
}

const CScraperUrl::SUrlEntry CScraperUrl::GetFirstThumb(const std::string& type) const
{
  const auto url = std::find_if(m_url.begin(), m_url.end(), [type](const SUrlEntry& url) {
    return url.m_type == UrlType::General && (type.empty() || url.m_aspect == type);
  });
  if (url != m_url.end())
    return *url;

  return {};
}

const CScraperUrl::SUrlEntry CScraperUrl::GetSeasonThumb(int season, const std::string& type) const
{
  const auto url = std::find_if(m_url.begin(), m_url.end(), [season, type](const SUrlEntry& url) {
    return url.m_type == UrlType::Season && url.m_season == season &&
           (type.empty() || type == "thumb" || url.m_aspect == type);
  });
  if (url != m_url.end())
    return *url;

  return {};
}

unsigned int CScraperUrl::GetMaxSeasonThumb() const
{
  unsigned int maxSeason = 0;
  for (const auto& url : m_url)
  {
    if (url.m_type == UrlType::Season && url.m_season > 0 &&
        static_cast<unsigned int>(url.m_season) > maxSeason)
      maxSeason = url.m_season;
  }
  return maxSeason;
}

bool CScraperUrl::Get(const SUrlEntry& scrURL,
                      std::string& strHTML,
                      XFILE::CCurlFile& http,
                      const std::string& cacheContext)
{
  CURL url(scrURL.m_url);
  http.SetReferer(scrURL.m_spoof);
  std::string strCachePath;

  if (!scrURL.m_cache.empty())
  {
    strCachePath = URIUtils::AddFileToFolder(
        CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->m_cachePath, "scrapers",
        cacheContext, scrURL.m_cache);
    if (XFILE::CFile::Exists(strCachePath))
    {
      XFILE::CFile file;
      XFILE::auto_buffer buffer;
      if (file.LoadFile(strCachePath, buffer) > 0)
      {
        strHTML.assign(buffer.get(), buffer.length());
        return true;
      }
    }
  }

  auto strHTML1 = strHTML;

  if (scrURL.m_post)
  {
    std::string strOptions = url.GetOptions();
    strOptions = strOptions.substr(1);
    url.SetOptions("");

    if (!http.Post(url.Get(), strOptions, strHTML1))
      return false;
  }
  else if (!http.Get(url.Get(), strHTML1))
    return false;

  strHTML = strHTML1;

  const auto mimeType = http.GetProperty(XFILE::FILE_PROPERTY_MIME_TYPE);
  CMime::EFileType ftype = CMime::GetFileTypeFromMime(mimeType);
  if (ftype == CMime::FileTypeUnknown)
    ftype = CMime::GetFileTypeFromContent(strHTML);

  if (ftype == CMime::FileTypeZip || ftype == CMime::FileTypeGZip)
  {
    XFILE::CZipFile file;
    std::string strBuffer;
    auto iSize = file.UnpackFromMemory(
        strBuffer, strHTML, scrURL.m_isgz); // FIXME: use FileTypeGZip instead of scrURL.m_isgz?
    if (iSize > 0)
    {
      strHTML = strBuffer;
      CLog::Log(LOGDEBUG, "{}: Archive \"{}\" was unpacked in memory", __FUNCTION__, scrURL.m_url);
    }
    else
      CLog::Log(LOGWARNING, "{}: \"{}\" looks like archive but cannot be unpacked", __FUNCTION__,
                scrURL.m_url);
  }

  const auto reportedCharset = http.GetProperty(XFILE::FILE_PROPERTY_CONTENT_CHARSET);
  if (ftype == CMime::FileTypeHtml)
  {
    std::string realHtmlCharset, converted;
    if (!CCharsetDetection::ConvertHtmlToUtf8(strHTML, converted, reportedCharset, realHtmlCharset))
      CLog::Log(LOGWARNING,
                "{}: Can't find precise charset for HTML \"{}\", using \"{}\" as fallback",
                __FUNCTION__, scrURL.m_url, realHtmlCharset);
    else
      CLog::Log(LOGDEBUG, "{}: Using \"{}\" charset for HTML \"{}\"", __FUNCTION__, realHtmlCharset,
                scrURL.m_url);

    strHTML = converted;
  }
  else if (ftype == CMime::FileTypeXml)
  {
    CXBMCTinyXML xmlDoc;
    xmlDoc.Parse(strHTML, reportedCharset);

    const auto realXmlCharset = xmlDoc.GetUsedCharset();
    if (!realXmlCharset.empty())
    {
      CLog::Log(LOGDEBUG, "{}: Using \"{}\" charset for XML \"{}\"", __FUNCTION__, realXmlCharset,
                scrURL.m_url);
      std::string converted;
      g_charsetConverter.ToUtf8(realXmlCharset, strHTML, converted);
      strHTML = converted;
    }
  }
  else if (ftype == CMime::FileTypePlainText ||
           StringUtils::EqualsNoCase(mimeType.substr(0, 5), "text/"))
  {
    std::string realTextCharset;
    std::string converted;
    CCharsetDetection::ConvertPlainTextToUtf8(strHTML, converted, reportedCharset, realTextCharset);
    strHTML = converted;
    if (reportedCharset != realTextCharset)
      CLog::Log(LOGWARNING,
                "{}: Using \"{}\" charset for plain text \"{}\" instead of server reported \"{}\" "
                "charset",
                __FUNCTION__, realTextCharset, scrURL.m_url, reportedCharset);
    else
      CLog::Log(LOGDEBUG, "{}: Using \"{}\" charset for plain text \"{}\"", __FUNCTION__,
                realTextCharset, scrURL.m_url);
  }
  else if (!reportedCharset.empty())
  {
    CLog::Log(LOGDEBUG, "{}: Using \"{}\" charset for \"{}\"", __FUNCTION__, reportedCharset,
              scrURL.m_url);
    if (reportedCharset != "UTF-8")
    {
      std::string converted;
      g_charsetConverter.ToUtf8(reportedCharset, strHTML, converted);
      strHTML = converted;
    }
  }
  else
    CLog::Log(LOGDEBUG, "{}: Using content of \"{}\" as binary or text with \"UTF-8\" charset",
              __FUNCTION__, scrURL.m_url);

  if (!scrURL.m_cache.empty())
  {
    const auto strCachePath = URIUtils::AddFileToFolder(
        CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->m_cachePath, "scrapers",
        cacheContext, scrURL.m_cache);
    XFILE::CFile file;
    if (!file.OpenForWrite(strCachePath, true) ||
        file.Write(strHTML.data(), strHTML.size()) != static_cast<ssize_t>(strHTML.size()))
      return false;
  }
  return true;
}

// XML format is of strUrls is:
// <TAG><url>...</url>...</TAG> (parsed by ParseElement) or <url>...</url> (ditto)
bool CScraperUrl::ParseEpisodeGuide(std::string strUrls)
{
  if (strUrls.empty())
    return false;

  // ok, now parse the xml file
  CXBMCTinyXML doc;
  /* strUrls is coming from internal sources so strUrls is always in UTF-8 */
  doc.Parse(strUrls, TIXML_ENCODING_UTF8);
  if (doc.RootElement() != nullptr)
  {
    TiXmlHandle docHandle(&doc);
    auto link = docHandle.FirstChild("episodeguide").Element();
    if (link->FirstChildElement("url"))
    {
      for (link = link->FirstChildElement("url"); link; link = link->NextSiblingElement("url"))
        ParseElement(link);
    }
    else if (link->FirstChild() && link->FirstChild()->Value())
      ParseElement(link);
  }
  else
    return false;

  return true;
}

void CScraperUrl::AddElement(std::string url,
                             std::string aspect,
                             std::string preview,
                             std::string referrer,
                             std::string cache,
                             bool post,
                             bool isgz,
                             int season)
{
  TiXmlElement thumb("thumb");
  thumb.SetAttribute("spoof", referrer);
  thumb.SetAttribute("cache", cache);
  if (post)
    thumb.SetAttribute("post", "yes");
  if (isgz)
    thumb.SetAttribute("gzip", "yes");
  if (season >= 0)
  {
    thumb.SetAttribute("season", StringUtils::Format("%i", season));
    thumb.SetAttribute("type", "season");
  }
  thumb.SetAttribute("aspect", aspect);
  thumb.SetAttribute("preview", preview);
  TiXmlText text(url);
  thumb.InsertEndChild(text);
  m_data << thumb;
  SUrlEntry nUrl;
  nUrl.m_url = url;
  nUrl.m_spoof = referrer;
  nUrl.m_post = post;
  nUrl.m_isgz = isgz;
  nUrl.m_cache = cache;
  if (season >= 0)
  {
    nUrl.m_type = UrlType::Season;
    nUrl.m_season = season;
  }
  else
    nUrl.m_type = UrlType::General;

  nUrl.m_aspect = aspect;

  m_url.push_back(nUrl);
}

std::string CScraperUrl::GetThumbURL(const CScraperUrl::SUrlEntry& entry)
{
  if (entry.m_spoof.empty())
    return entry.m_url;

  return entry.m_url + "|Referer=" + CURL::Encode(entry.m_spoof);
}

void CScraperUrl::GetThumbURLs(std::vector<std::string>& thumbs,
                               const std::string& type,
                               int season,
                               bool unique) const
{
  for (const auto& url : m_url)
  {
    if (url.m_aspect == type || type.empty() || url.m_aspect.empty())
    {
      if ((url.m_type == CScraperUrl::UrlType::General && season == -1) ||
          (url.m_type == CScraperUrl::UrlType::Season && url.m_season == season))
      {
        std::string thumbUrl = GetThumbURL(url);
        if (!unique || std::find(thumbs.begin(), thumbs.end(), thumbUrl) == thumbs.end())
          thumbs.push_back(thumbUrl);
      }
    }
  }
}
