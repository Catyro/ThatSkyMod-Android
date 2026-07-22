#include <core/UpdateChecker.h>
#include <network/HttpClient.h>
#include <utils/logging/log.h>
#include <Cipher/Cipher.h>
#include <utils/common/obfuscate.h>

#include <thread>
#include <mutex>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <cstdio>
#include <memory>
#include <initializer_list>

namespace tsm {
    namespace core {

        namespace {
            constexpr int API_TIMEOUT_SECONDS = 10;
            constexpr int DOWNLOAD_TIMEOUT_SECONDS = 60;
            constexpr int MAX_RETRIES = 3;
            constexpr int MAX_REDIRECTS = 5;
            constexpr size_t MAX_DOWNLOAD_SIZE = 100 * 1024 * 1024;

            const char* GITHUB_API_HOST = _O("api.github.com");
            const char* GITHUB_RELEASES_ENDPOINT = _O("/repos/Catyro/ThatSkyMod-Android/releases/latest");
            const char* DOWNLOAD_URL = _O("https://github.com/Catyro/ThatSkyMod-Android/releases/latest/download/libTSM.so");
            const char* DEFAULT_FILES_DIR = _O("/data/data/git.artdeell.skymodloader/files");

            std::mutex g_updateMutex;

            std::string ExtractJsonValue(const std::string& json, const std::string& key,
                size_t startPos = 0) {
                if (json.empty() || key.empty()) return _OS("");

                std::string searchKey = _OS("\"") + key + _OS("\"");
                size_t pos = json.find(searchKey, startPos);
                if (pos == std::string::npos) return _OS("");

                pos = json.find(':', pos);
                if (pos == std::string::npos) return _OS("");

                ++pos;
                while (pos < json.length() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;

                if (json.substr(pos, 4) == _OS("null")) return _OS("");

                if (pos >= json.length() || json[pos] != '"') return _OS("");
                ++pos;

                size_t end = pos;
                while (end < json.length()) {
                    if (json[end] == '"' && (end == pos || json[end - 1] != '\\')) break;
                    ++end;
                }

                if (end >= json.length()) return _OS("");
                return json.substr(pos, end - pos);
            }

            struct UrlComponents {
                std::string scheme;
                std::string host;
                std::string path;
                bool valid = false;
            };

            UrlComponents ParseUrl(const std::string& url) {
                UrlComponents result;
                if (url.empty()) return result;

                std::string work = url;
                auto schemePos = work.find(_OS("://"));
                if (schemePos == std::string::npos) return result;
                result.scheme = work.substr(0, schemePos);
                std::transform(result.scheme.begin(), result.scheme.end(), result.scheme.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                work = work.substr(schemePos + 3);

                auto slashPos = work.find('/');
                if (slashPos == std::string::npos) {
                    result.host = work;
                    result.path = _OS("/");
                }
                else {
                    result.host = work.substr(0, slashPos);
                    result.path = work.substr(slashPos);
                }

                while (result.path.length() > 1 && result.path.back() == '/') {
                    result.path.pop_back();
                }

                std::transform(result.host.begin(), result.host.end(), result.host.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                result.valid = !result.scheme.empty() && !result.host.empty()
                    && result.host.find('@') == std::string::npos
                    && result.host.find(':') == std::string::npos;
                return result;
            }

            bool IsAllowedGitHubHost(const std::string& host) {
                if (host == _OS("github.com")
                    || host == _OS("api.github.com")
                    || host == _OS("objects.githubusercontent.com")
                    || host == _OS("release-assets.githubusercontent.com")) {
                    return true;
                }
                const std::string suffix = _OS(".githubusercontent.com");
                return host.length() > suffix.length()
                    && host.compare(host.length() - suffix.length(), suffix.length(), suffix) == 0;
            }

            bool IsCatyroReleaseDownload(const std::string& url) {
                const UrlComponents components = ParseUrl(url);
                if (!components.valid || components.scheme != _OS("https")
                    || components.host != _OS("github.com")) {
                    return false;
                }
                const std::string releasePrefix = _OS("/Catyro/ThatSkyMod-Android/releases/download/");
                const std::string latestPrefix = _OS("/Catyro/ThatSkyMod-Android/releases/latest/download/");
                return components.path.rfind(releasePrefix, 0) == 0
                    || components.path.rfind(latestPrefix, 0) == 0;
            }

            std::string ExtractReleaseAssetUrl(const std::string& json, const Version& version) {
                if (json.empty() || !version.IsValid()) return _OS("");

                const std::string desiredName = _OS("libTSM v") + version.ToString() + _OS(".so");
                const std::string normalizedName = _OS("libTSM.v") + version.ToString() + _OS(".so");
                const std::string fallbackName = _OS("libTSM.so");
                for (const std::string& assetName : { desiredName, normalizedName, fallbackName }) {
                    size_t cursor = 0;
                    while (cursor < json.length()) {
                        const size_t namePos = json.find(_OS("\"name\""), cursor);
                        if (namePos == std::string::npos) break;
                        const std::string name = ExtractJsonValue(json, _OS("name"), namePos);
                        if (name == assetName) {
                            const size_t urlPos = json.find(_OS("\"browser_download_url\""), namePos);
                            if (urlPos != std::string::npos && urlPos - namePos <= 64 * 1024) {
                                const std::string url = ExtractJsonValue(
                                    json, _OS("browser_download_url"), urlPos);
                                if (IsCatyroReleaseDownload(url)) return url;
                            }
                        }
                        cursor = namePos + 6;
                    }
                }
                return _OS("");
            }

            class FileHandle {
            public:
                explicit FileHandle(const std::string& path, std::ios::openmode mode)
                    : m_file(path, mode) {
                }

                bool IsOpen() const { return m_file.is_open(); }
                bool IsGood() const { return m_file.good(); }

                std::ofstream& Get() { return m_file; }

                ~FileHandle() {
                    if (m_file.is_open()) m_file.close();
                }

            private:
                std::ofstream m_file;
            };

            std::string GetHeader(const std::map<std::string, std::string>& headers,
                const std::string& name) {
                auto it = headers.find(name);
                if (it != headers.end()) return it->second;

                std::string lower = name;
                std::transform(lower.begin(), lower.end(), lower.begin(),
                    [](unsigned char c) { return std::tolower(c); });
                it = headers.find(lower);
                return (it != headers.end()) ? it->second : _OS("");
            }
        }


        Version Version::Parse(const std::string& str) {
            Version v;
            if (str.empty()) return v;

            std::string tag = str;
            std::string s = tag;
            if (tag.rfind(_OS("sky-"), 0) == 0 || tag.rfind(_OS("SKY-"), 0) == 0) {
                s = tag.substr(4);
            }
            if (s.empty()) return v;
            if (s[0] == 'v' || s[0] == 'V') s = s.substr(1);
            if (s.empty()) return v;

            auto parseToken = [](const std::string& t) -> int {
                if (t.empty()) return 0;
                try {
                    size_t pos;
                    int val = std::stoi(t, &pos);
                    return (pos == t.length()) ? val : 0;
                }
                catch (...) {
                    return 0;
                }
                };

            std::istringstream iss(s);
            std::string token;

            if (std::getline(iss, token, '.')) v.major = parseToken(token);
            if (std::getline(iss, token, '.')) v.minor = parseToken(token);
            if (std::getline(iss, token, '.')) v.patch = parseToken(token);

            return v;
        }

        bool Version::operator>(const Version& o)  const { return major != o.major ? major > o.major : minor != o.minor ? minor > o.minor : patch > o.patch; }
        bool Version::operator>=(const Version& o) const { return !(*this < o); }
        bool Version::operator<(const Version& o)  const { return o > *this; }
        bool Version::operator<=(const Version& o) const { return !(*this > o); }
        bool Version::operator==(const Version& o) const { return major == o.major && minor == o.minor && patch == o.patch; }
        bool Version::operator!=(const Version& o) const { return !(*this == o); }

        std::string Version::ToString() const {
            return std::to_string(major) + _OS(".") + std::to_string(minor) + _OS(".") + std::to_string(patch);
        }

        bool Version::IsValid() const {
            return major > 0 || minor > 0 || patch > 0;
        }


        UpdateChecker& UpdateChecker::Get() {
            static UpdateChecker instance;
            return instance;
        }

        void UpdateChecker::CheckForUpdates() {
            {
                std::lock_guard<std::mutex> lock(g_updateMutex);
                if (m_checkStarted) return;
                m_checkStarted = true;
                m_checking = true;
                m_currentVersion = GetCurrentVersionFromConfig();
            }

            std::thread([this]() { CheckForUpdatesInternal(); }).detach();
        }

        bool        UpdateChecker::HasUpdate()        const { std::lock_guard<std::mutex> lock(g_updateMutex); return m_hasUpdate; }
        std::string UpdateChecker::GetLatestVersion() const { std::lock_guard<std::mutex> lock(g_updateMutex); return m_latestVersion.ToString(); }
        std::string UpdateChecker::GetCurrentVersion()const { std::lock_guard<std::mutex> lock(g_updateMutex); return m_currentVersion.ToString(); }
        std::string UpdateChecker::GetDownloadUrl()   const {
            std::lock_guard<std::mutex> lock(g_updateMutex);
            return m_downloadUrl.empty() ? DOWNLOAD_URL : m_downloadUrl;
        }
        bool        UpdateChecker::IsChecking()       const { std::lock_guard<std::mutex> lock(g_updateMutex); return m_checking; }

        void UpdateChecker::MarkNotificationSeen() {
            std::lock_guard<std::mutex> lock(g_updateMutex);
            m_notificationSeen = true;
        }

        bool UpdateChecker::ShouldShowNotification() const {
            std::lock_guard<std::mutex> lock(g_updateMutex);
            return m_hasUpdate && !m_notificationSeen;
        }

        bool UpdateChecker::ConsumeJustInstalledFlag() {
            std::lock_guard<std::mutex> lock(g_updateMutex);
            bool was = m_updateJustInstalled;
            m_updateJustInstalled = false;
            return was;
        }


        void UpdateChecker::CheckForUpdatesInternal() {
            std::string downloadUrl;
            Version latestVersion = FetchLatestVersion(&downloadUrl);
            bool shouldInstall = false;

            {
                std::lock_guard<std::mutex> lock(g_updateMutex);
                m_checking = false;
                m_latestVersion = latestVersion;

                m_hasUpdate = latestVersion.IsValid() && (latestVersion > m_currentVersion);
                if (!downloadUrl.empty()) m_downloadUrl = downloadUrl;
                shouldInstall = m_hasUpdate;
            }

            if (shouldInstall) InstallLatest();
        }


        Version UpdateChecker::FetchLatestVersion(std::string* downloadUrl) {
            try {
                tsm::network::HttpClient client(GITHUB_API_HOST, 443);
                client.SetTimeout(API_TIMEOUT_SECONDS);

                tsm::network::HttpRequest request;
                request.endpoint = GITHUB_RELEASES_ENDPOINT;
                request.headers[_OS("Accept")] = _OS("application/vnd.github.v3+json");
                request.headers[_OS("User-Agent")] = _OS("ThatSkyMod-UpdateChecker");

                tsm::network::HttpResponse response = client.Get(request, MAX_RETRIES, 3);
                if (!response.IsSuccess()) return {};

                std::string tagName = ExtractJsonValue(response.body, _OS("tag_name"));
                if (tagName.empty()) return {};

                Version version = Version::Parse(tagName);
                if (!version.IsValid()) return {};
                if (downloadUrl) *downloadUrl = ExtractReleaseAssetUrl(response.body, version);
                return version;

            }
            catch (...) {}

            return {};
        }

        Version UpdateChecker::GetCurrentVersionFromConfig() {
            return Version{
#ifdef TSM_COMPAT_VERSION_MAJOR
                TSM_COMPAT_VERSION_MAJOR,
                TSM_COMPAT_VERSION_MINOR,
                TSM_COMPAT_VERSION_PATCH
#else
                0, 0, 0
#endif
            };
        }


        void UpdateChecker::InstallLatest() {
            {
                std::lock_guard<std::mutex> lock(g_updateMutex);
                if (m_installRunning) return;
                m_installRunning = true;
            }

            std::thread([this]() { InstallLatestInternal(); }).detach();
        }

        void UpdateChecker::InstallLatestInternal() {
            bool        success = false;
            bool        hadUpdate = false;

            {
                std::lock_guard<std::mutex> lock(g_updateMutex);
                hadUpdate = m_hasUpdate;
            }

            try {
                std::string filesDir = DetermineFilesDirectory();
                std::string targetPath = filesDir + _OS("/mods/libTSM.so");
                std::string tempPath = targetPath + _OS(".tmp");

                if (!DownloadLatestToFile(tempPath)) {
                    throw std::runtime_error(_OS("Download failed"));
                }

                std::remove(targetPath.c_str());

                if (std::rename(tempPath.c_str(), targetPath.c_str()) != 0) {
                    std::remove(tempPath.c_str());
                    throw std::runtime_error(_OS("Failed to move updated binary into place"));
                }

                success = true;

            }
            catch (const std::exception&) {
            }
            catch (...) {}

            {
                std::lock_guard<std::mutex> lock(g_updateMutex);
                m_installRunning = false;
                if (success) {
                    m_updateJustInstalled = hadUpdate;
                    m_hasUpdate = false;
                    m_notificationSeen = true;
                }
            }
        }


        std::string UpdateChecker::DetermineFilesDirectory() {
            const char* cfgPath = Cipher::getConfigPath();
            if (!cfgPath || !*cfgPath) return DEFAULT_FILES_DIR;

            std::string filesDir(cfgPath);

            auto configPos = filesDir.rfind(_OS("/config"));
            if (configPos != std::string::npos) {
                filesDir.erase(configPos);
            }
            else {
                auto pos = filesDir.find_last_of("/\\");
                if (pos != std::string::npos && pos > 0) filesDir.erase(pos);
            }

            while (filesDir.length() > 1 && filesDir.back() == '/') filesDir.pop_back();

            return filesDir;
        }

        bool UpdateChecker::DownloadLatestToFile(const std::string& path) {
            try {
                std::string currentUrl = GetDownloadUrl();
                if (currentUrl.empty()) return false;

                for (int redirectCount = 0; redirectCount < MAX_REDIRECTS; ++redirectCount) {
                    UrlComponents components = ParseUrl(currentUrl);
                    if (!components.valid || components.scheme != _OS("https")
                        || !IsAllowedGitHubHost(components.host)) return false;

                    tsm::network::HttpClient client(components.host, 443);
                    client.SetTimeout(DOWNLOAD_TIMEOUT_SECONDS);

                    tsm::network::HttpRequest request;
                    request.endpoint = components.path;
                    request.headers[_OS("User-Agent")] = _OS("ThatSkyMod-UpdateChecker");

                    tsm::network::HttpResponse response = client.Get(request, MAX_RETRIES, 3);

                    if (response.status >= 301 && response.status <= 308) {
                        std::string location = GetHeader(response.headers, _OS("Location"));
                        if (location.empty()) return false;
                        if (redirectCount == 0 && !IsCatyroReleaseDownload(currentUrl)) return false;
                        currentUrl = location;
                        continue;
                    }

                    if (!response.IsSuccess())       return false;
                    if (response.body.empty())        return false;
                    if (response.body.size() > MAX_DOWNLOAD_SIZE) return false;

                    FileHandle file(path, std::ios::binary | std::ios::trunc);
                    if (!file.IsOpen()) return false;

                    file.Get().write(response.body.data(),
                        static_cast<std::streamsize>(response.body.size()));

                    return file.IsGood();
                }

                return false;

            }
            catch (...) {}

            return false;
        }

    }
}
