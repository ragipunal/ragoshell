#pragma once

#include <stdexcept>
#include <string>

namespace RagoShell
{
    /**
     * @brief Simple host:port representation.
     *
     * Replaces mongo::HostAndPort from the old embedded MongoDB fork.
     * Accepts the same input forms: "host:port", "host", "[ipv6]:port".
     */
    class HostAndPort
    {
    public:
        HostAndPort() {}

        HostAndPort(const std::string &host, int port)
            : _host(host), _port(port)
        {
            // Strip surrounding brackets from IPv6 literals
            if (_host.size() >= 2 && _host.front() == '[' && _host.back() == ']')
                _host = _host.substr(1, _host.size() - 2);
        }

        /**
         * @brief Parses "host:port", "[ipv6]:port" or bare "host"/"ipv6".
         * Throws std::invalid_argument when a port suffix is malformed.
         */
        explicit HostAndPort(const std::string &str)
        {
            // Bracketed IPv6: [addr] or [addr]:port
            if (str.front() == '[') {
                const std::string::size_type close = str.find(']');
                if (close == std::string::npos)
                    throw std::invalid_argument("Malformed IPv6 address: " + str);
                _host = str.substr(1, close - 1);
                if (close + 1 < str.size()) {
                    if (str[close + 1] != ':')
                        throw std::invalid_argument("Malformed host:port: " + str);
                    _port = parsePort(str.substr(close + 2));
                }
                return;
            }

            const std::string::size_type colon = str.rfind(':');
            if (colon == std::string::npos) {
                _host = str;          // bare host or unbracketed IPv6, no port
            } else if (str.find(':') != colon) {
                _host = str;          // multiple colons: unbracketed IPv6, no port
            } else {
                _host = str.substr(0, colon);
                _port = parsePort(str.substr(colon + 1));
            }
        }

        bool empty() const { return _host.empty(); }
        const std::string &host() const { return _host; }
        int port() const { return _port; }
        bool hasPort() const { return _port >= 0; }

        std::string toString() const
        {
            if (_host.empty())
                return "";
            std::string h = _host;
            if (h.find(':') != std::string::npos)
                h = '[' + h + ']';
            if (_port < 0)
                return h;
            return h + ':' + std::to_string(_port);
        }

        bool operator==(const HostAndPort &other) const
        {
            return _host == other._host && _port == other._port;
        }
        bool operator!=(const HostAndPort &other) const { return !(*this == other); }
        bool operator<(const HostAndPort &other) const
        {
            return _host < other._host || (_host == other._host && _port < other._port);
        }

    private:
        static int parsePort(const std::string &s)
        {
            std::size_t pos = 0;
            int port = 0;
            try {
                port = std::stoi(s, &pos);
            } catch (...) {
                throw std::invalid_argument("Malformed port in: " + s);
            }
            if (pos != s.size() || port < 0 || port > 65535)
                throw std::invalid_argument("Malformed port in: " + s);
            return port;
        }

        std::string _host;
        int _port = -1;   // -1: no port specified
    };
}
