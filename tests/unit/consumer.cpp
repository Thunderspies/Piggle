#include <gtest/gtest.h>

extern "C" int piggle_c11_contract(void);

TEST(Consumers, C11HeaderAndLinkage)
{
	EXPECT_EQ(piggle_c11_contract(), 0);
}
