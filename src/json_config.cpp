// IP Camera JSON 配置加载（F8）——极简解析，仅支持 sparkcap.json 固定 schema。
// 不引入第三方 JSON 库（最小够用）：文件缺失/格式错误一律返回空，不影响系统设备枚举。

#include "json_config.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace sparkcap {
namespace {

class JsonParser {
public:
    explicit JsonParser(const std::string& text)
        : m_text(text)
    {
    }

    // 解析顶层对象并提取 ip_cameras 数组
    bool parseCameras(std::vector<sparkcap_device_info_t>& out)
    {
        skipWs();
        if (!eat('{'))
            return false;
        skipWs();
        while (!eat('}')) {
            std::string key;
            if (!parseString(key))
                return false;
            skipWs();
            if (!eat(':'))
                return false;
            skipWs();
            if (key == "ip_cameras") {
                if (!parseCameraArray(out))
                    return false;
            } else if (!skipValue()) {
                return false;
            }
            skipWs();
            eat(',');  // 允许尾随逗号（宽容）
            skipWs();
        }
        return true;
    }

private:
    // ---- 词法 ----

    void skipWs()
    {
        while (m_pos < m_text.size()) {
            const char c = m_text[m_pos];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
                ++m_pos;
            else
                break;
        }
    }

    bool eat(char c)
    {
        if (m_pos < m_text.size() && m_text[m_pos] == c) {
            ++m_pos;
            return true;
        }
        return false;
    }

    bool parseString(std::string& out)
    {
        out.clear();
        if (!eat('"'))
            return false;
        while (m_pos < m_text.size()) {
            char c = m_text[m_pos++];
            if (c == '"')
                return true;
            if (c == '\\' && m_pos < m_text.size()) {
                const char e = m_text[m_pos++];
                switch (e) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'n':  out += '\n'; break;
                case 't':  out += '\t'; break;
                case 'r':  out += '\r'; break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                default:   out += e;    break;  // \uXXXX 不支持（schema 用不到）
                }
            } else {
                out += c;
            }
        }
        return false;  // 未闭合
    }

    // 跳过任意 JSON 值（本 schema 只需忽略未知字段）
    bool skipValue()
    {
        skipWs();
        if (m_pos >= m_text.size())
            return false;
        const char c = m_text[m_pos];
        if (c == '"') {
            std::string tmp;
            return parseString(tmp);
        }
        if (c == '{' || c == '[') {
            const char open = c;
            const char close = (c == '{') ? '}' : ']';
            int depth = 0;
            while (m_pos < m_text.size()) {
                const char d = m_text[m_pos++];
                if (d == '"') {  // 跳过字符串内容
                    std::string tmp;
                    if (!parseString(tmp))
                        return false;
                    continue;
                }
                if (d == open)
                    ++depth;
                else if (d == close) {
                    --depth;
                    if (depth == 0)
                        return true;
                }
            }
            return false;
        }
        // 数值 / true / false / null：读到分隔符
        while (m_pos < m_text.size()) {
            const char d = m_text[m_pos];
            if (d == ',' || d == '}' || d == ']' || d == ' ' || d == '\t' ||
                d == '\r' || d == '\n')
                break;
            ++m_pos;
        }
        return true;
    }

    // ---- ip_cameras 数组 ----

    bool parseCameraArray(std::vector<sparkcap_device_info_t>& out)
    {
        if (!eat('['))
            return false;
        skipWs();
        while (!eat(']')) {
            sparkcap_device_info_t info = {};
            std::string name;
            std::string url;
            std::string transport;
            if (!parseCameraObject(name, url, transport))
                return false;
            if (!url.empty() && _strnicmp(url.c_str(), "rtsp://", 7) == 0) {
                snprintf(info.name, sizeof(info.name), "%s", name.c_str());  // UTF-8 直传
                snprintf(info.path, sizeof(info.path), "%s", url.c_str());
                info.backend = SPARKCAP_BACKEND_FFMPEG;
                out.push_back(info);
                (void)transport;  // 接线随 createSession 的 transport 参数细化（M3 收尾）
            }
            skipWs();
            eat(',');
            skipWs();
        }
        return true;
    }

    bool parseCameraObject(std::string& name, std::string& url, std::string& transport)
    {
        if (!eat('{'))
            return false;
        skipWs();
        while (!eat('}')) {
            std::string key;
            if (!parseString(key))
                return false;
            skipWs();
            if (!eat(':'))
                return false;
            skipWs();
            if (key == "name")
                parseString(name);
            else if (key == "url")
                parseString(url);
            else if (key == "transport")
                parseString(transport);
            else if (!skipValue())
                return false;
            skipWs();
            eat(',');
            skipWs();
        }
        return true;
    }

    const std::string& m_text;
    size_t m_pos = 0;
};

} // namespace

std::vector<sparkcap_device_info_t> loadJsonCamerasFromFile(const char* path)
{
    std::vector<sparkcap_device_info_t> cameras;
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || f == nullptr)
        return cameras;  // 无配置文件：正常（只用系统设备）

    std::string text;
    char buf[4096];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        text.append(buf, n);
    fclose(f);

    JsonParser parser(text);
    parser.parseCameras(cameras);  // 失败返回已解析部分（宽容）
    return cameras;
}

std::vector<sparkcap_device_info_t> loadJsonCameras()
{
    return loadJsonCamerasFromFile("sparkcap.json");
}

} // namespace sparkcap
