#include "HttpParse.h"
#include <cctype>
#include <cstring>

namespace home2hook {

    bool StartsWithHttpMethod(const char* data, size_t length)
    {
        static const char* kMethods[] = {
            "GET ", "POST ", "PUT ", "DELETE ", "HEAD ", "PATCH ", "OPTIONS "
        };
        for (const char* method : kMethods)
        {
            size_t methodLength = std::strlen(method);
            if (length >= methodLength && std::memcmp(data, method, methodLength) == 0)
                return true;
        }
        return false;
    }

    std::string UrlDecode(const std::string& text)
    {
        auto hexValue = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        std::string out;
        out.reserve(text.size());
        for (size_t i = 0; i < text.size(); ++i)
        {
            char c = text[i];
            if (c == '+')
            {
                out += ' ';
            }
            else if (c == '%' && i + 2 < text.size())
            {
                int hi = hexValue(text[i + 1]);
                int lo = hexValue(text[i + 2]);
                if (hi >= 0 && lo >= 0)
                {
                    out += static_cast<char>((hi << 4) | lo);
                    i += 2;
                }
                else
                {
                    out += c;
                }
            }
            else
            {
                out += c;
            }
        }
        return out;
    }

    static bool GetFormField(const std::string& body, const std::string& name, std::string& valueOut)
    {
        size_t pos = 0;
        while (pos <= body.size())
        {
            size_t amp = body.find('&', pos);
            std::string pair = (amp == std::string::npos)
                                   ? body.substr(pos)
                                   : body.substr(pos, amp - pos);
            size_t eq = pair.find('=');
            if (eq != std::string::npos && pair.compare(0, eq, name) == 0)
            {
                valueOut = pair.substr(eq + 1);
                return true;
            }

            if (amp == std::string::npos) break;
            pos = amp + 1;
        }
        return false;
    }

    // Read a header attribute value (boundary / name / filename) tolerating UE's non-standard whitespace around '=' (`boundary =...`, `name = "world_id"`, but also the inconsistent `name="file"`).
    // key is matched as a whole word so "name" never matches inside "filename" for example.
    //  lower is the lowercased copy used for the case-insensitive search, and the value is taken from the original, case-preserving copy. Returns "" if the key is absent.
    static std::string ExtractHeaderAttr(const std::string& headers, const std::string& lower, const std::string& key)
    {
        size_t search = 0;
        while (true)
        {
            size_t p = lower.find(key, search);
            if (p == std::string::npos) return std::string();

            // Reject a match that is part of a longer identifier (e.g. "name" inside "filename").
            bool wordStart = (p == 0) || !std::isalpha(static_cast<unsigned char>(lower[p - 1]));

            size_t q = p + key.size();
            while (q < headers.size() && (headers[q] == ' ' || headers[q] == '\t'))
            {
                ++q; // optional whitespace before '='
            }
            
            if (wordStart && q < headers.size() && headers[q] == '=')
            {
                ++q; // past '='
                while (q < headers.size() && (headers[q] == ' ' || headers[q] == '\t'))
                {
                    ++q; // optional whitespace after '='
                }

                if (q < headers.size() && headers[q] == '"')
                {
                    size_t end = headers.find('"', q + 1);
                    if (end == std::string::npos) return std::string();

                    return headers.substr(q + 1, end - q - 1);
                }
                size_t end = headers.find_first_of(";\r\n ", q);
                return headers.substr(q, (end == std::string::npos ? headers.size() : end) - q);
            }
            search = p + 1;
        }
    }

    bool ParseMultipartUpload(const std::string& request, std::string& worldIdOut, std::string& fileBytesOut, std::string& extOut)
    {
        worldIdOut.clear();
        fileBytesOut.clear();
        extOut.clear();

        size_t headerEnd = request.find("\r\n\r\n");
        if (headerEnd == std::string::npos) return false;

        std::string headers = request.substr(0, headerEnd);

        std::string headersLower = headers;
        for (auto& c : headersLower)
        {
            c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        }
        
        // UE sends `boundary =-----...` with a space before '=', and ExtractHeaderAttr tolerates it.
        std::string boundary = ExtractHeaderAttr(headers, headersLower, "boundary");
        if (boundary.empty()) return false;

        std::string body = request.substr(headerEnd + 4);
        std::string delim = "--" + boundary;

        size_t pos = body.find(delim);
        if (pos == std::string::npos) return false;

        pos += delim.size();

        while (pos < body.size())
        {
            if (body.compare(pos, 2, "--") == 0) break; // closing delimiter
            
            if (body.compare(pos, 2, "\r\n") == 0)
            {
                pos += 2;
            }

            size_t partHeaderEnd = body.find("\r\n\r\n", pos);
            if (partHeaderEnd == std::string::npos) break;

            std::string partHeaders = body.substr(pos, partHeaderEnd - pos);
            size_t contentStart = partHeaderEnd + 4;

            size_t nextDelim = body.find(delim, contentStart);
            if (nextDelim == std::string::npos) break;

            size_t contentEnd = nextDelim;
            if (contentEnd >= 2 && body.compare(contentEnd - 2, 2, "\r\n") == 0)
            {
                contentEnd -= 2; // strip the CRLF preceding the boundary
            }

            std::string content = body.substr(contentStart, contentEnd - contentStart);

            std::string partLower = partHeaders;
            for (auto& c : partLower)
            {
                c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
            }

            std::string name = ExtractHeaderAttr(partHeaders, partLower, "name");
            std::string filename = ExtractHeaderAttr(partHeaders, partLower, "filename");

            if (name == "world_id")
            {
                worldIdOut = content;
                while (!worldIdOut.empty() && (worldIdOut.back() == '\r' || worldIdOut.back() == '\n' || worldIdOut.back() == ' '))
                {
                    worldIdOut.pop_back();
                }
            }
            else if (name == "file")
            {
                fileBytesOut = content;
                size_t dot = filename.find_last_of('.');
                if (dot != std::string::npos)
                {
                    extOut = filename.substr(dot + 1);
                }
            }

            pos = nextDelim + delim.size();
        }

        return !worldIdOut.empty() && !fileBytesOut.empty();
    }

    bool ParseGraphqlRequest(const std::string& request, std::string& docId, std::string& variablesJson)
    {
        size_t lineEnd = request.find("\r\n");
        if (lineEnd == std::string::npos) return false;

        size_t graphqlPos = request.find("/graphql");
        if (graphqlPos == std::string::npos || graphqlPos > lineEnd) return false; // the path must appear on the request line

        size_t headerEnd = request.find("\r\n\r\n");
        if (headerEnd == std::string::npos) return false; // headers (and thus body) not fully written yet

        std::string body = request.substr(headerEnd + 4);
        if (body.empty()) return false;

        std::string rawDoc;
        if (!GetFormField(body, "doc_id", rawDoc)) return false;

        docId = UrlDecode(rawDoc);

        std::string rawVars;
        if (GetFormField(body, "variables", rawVars))
            variablesJson = UrlDecode(rawVars);
        else
            variablesJson.clear();
        return true;
    }

}
