#include "gtest/gtest.h"
#include "ragoshell/utils/StringOperations.h"

#include <string>

/* Example Test:
 *
 * TEST( [Test_Case_Name], [Test_Name] )
 * TEST( [Test_Case_Name], [UnitOfWorkName_ScenarioUnderTest_ExpectedBehavior] )
 * TEST( StringParserTests, NumberLeftOf_StringWithoutNumber_ReturnsFalse) {
    // ...
   }
*/

TEST(StringOperationsTests, captilizeFirstChar)
{
    // EXPECT_EQ("Abcc", RagoShell::captilizeFirstChar("abc")); // Simulating failing test
    EXPECT_EQ("Abc", RagoShell::captilizeFirstChar("abc")); // Simulating passing test
}