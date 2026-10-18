// The browser demo's wrapper, tested natively.
#include <gtest/gtest.h>

#include "../web/session.h"

namespace lob {

TEST(WebSessionTest, ReportsEventsAndTopOfBookAsJson) {
  web::Session s;
  std::string r1 = s.Submit("SELL", "LIMIT", 10100, 50);
  EXPECT_NE(r1.find("\"type\":\"rested\""), std::string::npos) << r1;
  EXPECT_NE(r1.find("\"asks\":[[10100,50,1]]"), std::string::npos) << r1;

  std::string r2 = s.Submit("BUY", "LIMIT", 10100, 20);
  EXPECT_NE(r2.find("\"type\":\"trade\""), std::string::npos) << r2;
  EXPECT_NE(r2.find("\"qty\":20"), std::string::npos) << r2;
  EXPECT_NE(r2.find("\"asks\":[[10100,30,1]]"), std::string::npos) << r2;

  std::string r3 = s.Submit("BUY", "FOK", 10100, 1000);
  EXPECT_NE(r3.find("fill-or-kill not fillable"), std::string::npos) << r3;

  std::string r4 = s.Cancel(1);
  EXPECT_NE(r4.find("\"type\":\"cancelled\""), std::string::npos) << r4;
  EXPECT_NE(r4.find("\"asks\":[]"), std::string::npos) << r4;
  EXPECT_TRUE(s.Invariants());
}

TEST(WebSessionTest, SimulationRunsOnTopOfInteractiveOrders) {
  web::Session s;
  s.Submit("BUY", "LIMIT", 9990, 10);
  std::string r = s.Simulate(20000);
  EXPECT_NE(r.find("\"simulated\":20000"), std::string::npos) << r;
  EXPECT_EQ(r.find("\"trades\":0,"), std::string::npos) << "simulation should trade";
  EXPECT_TRUE(s.Invariants());
  // A second batch uses a different seed and fresh ids.
  s.Simulate(20000);
  EXPECT_TRUE(s.Invariants());
  s.Reset();
  EXPECT_NE(s.Snapshot().find("\"orders\":0"), std::string::npos);
}

}  // namespace lob
