#pragma once
#include "ragoshell/core/utils/BsonUtils.h"

namespace RagoShell
{
    class MongoUser
    {
    public:
        typedef std::vector<std::string> RolesVector;

        explicit MongoUser(const float version, bsoncxx::document::view_or_value obj) :
            _version(version), _readOnly(false),
            _name(BsonUtils::getField<std::string>(obj, "user")),
            _userSource(BsonUtils::getField<std::string>(obj, "db"))
        {
            const std::string userDb = _userSource;
            const auto rolesField = obj.view()["roles"];
            if (rolesField && rolesField.type() == bsoncxx::type::k_array) {
                for (auto const& r : rolesField.get_array().value) {
                    if (r.type() == bsoncxx::type::k_document) {
                        const std::string rName = BsonUtils::getField<std::string>(r.get_document().view(), "role");
                        const std::string rDb = BsonUtils::getField<std::string>(r.get_document().view(), "db");
                        if (!rName.empty()) {
                            if (!rDb.empty() && !userDb.empty() && rDb != userDb)
                                _roles.push_back(rName + "@" + rDb);
                            else
                                _roles.push_back(rName);
                            if (_userSource.empty() && !rDb.empty())
                                _userSource = rDb;
                        }
                    } else if (r.type() == bsoncxx::type::k_string) {
                        const auto sv = r.get_string().value;
                        _roles.emplace_back(sv.data(), sv.size());
                    }
                }
            }

            const auto mechsField = obj.view()["mechanisms"];
            if (mechsField && mechsField.type() == bsoncxx::type::k_array) {
                for (auto const& m : mechsField.get_array().value) {
                    if (m.type() == bsoncxx::type::k_string) {
                        const auto sv = m.get_string().value;
                        _mechanisms.emplace_back(sv.data(), sv.size());
                    }
                }
            }
        };

        // Creates new user with empty attributes
        MongoUser(const float version) :
             _version(version), _readOnly(false), _roles(), _mechanisms() {}

        std::string name() const { return _name; }
        std::string password() const { return _password; }
        std::vector<std::string> roles() const { return _roles; }
        std::vector<std::string> mechanisms() const { return _mechanisms; }

        void setName(const std::string &name) { _name = name; }
        void setPassword(const std::string &pwd) { _password = pwd; }
        void setRoles(const std::vector<std::string> &roles) { _roles = roles; }
        void setMechanisms(const std::vector<std::string> &mechs) { _mechanisms = mechs; }

        std::string userSource() const { return _userSource; }
        void setUserSource(const std::string &source) { _userSource = source; }
        bool readOnly() const { return _readOnly; }
        void setReadOnly(bool readonly) { _readOnly = readonly; }
        bool overwrite() const { return _overwrite; }
        void setOverwrite(bool flag) { _overwrite = flag; }

        float version() const { return _version; }
        static constexpr float minimumSupportedVersion = 2.4f;
    private:
        float _version;
        std::string _name;
        bool _readOnly;
        bool _overwrite = false;
        RolesVector _roles;
        std::vector<std::string> _mechanisms;
        std::string _password;
        std::string _userSource;
    };
}