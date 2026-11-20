#include "gtest/gtest.h"
#include "ragoshell/utils/RagoCrypt.h"

#include <string>

/* Example Test:
 *
 * TEST( [Test_Case_Name], [Test_Name] )
 * TEST( [Test_Case_Name], [UnitOfWorkName_ScenarioUnderTest_ExpectedBehavior] )
 * TEST( StringParserTests, NumberLeftOf_StringWithoutNumber_ReturnsFalse) {
    // ...
   }
*/

TEST(RagoCrypt_CoreTests, encrypt_decrypt)
{  
    auto const pwds = {
        "Tyu_aBq",
        "_?asdfghjkl;'piop[.,/",
        ".?/`_@~!#$%^^&&*)_)_+=-",        
        "<>?/.,;':][p{}|\""
    };
    for (auto const& pwd : pwds) {
        const std::string encryptedPwd = RagoShell::RagoCrypt::encrypt(pwd);
        const std::string decryptedPwd = RagoShell::RagoCrypt::decrypt(encryptedPwd);
        EXPECT_EQ(pwd, decryptedPwd);
    }
}