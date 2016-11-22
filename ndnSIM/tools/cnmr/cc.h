#ifndef CC_H_
#define CC_H_

#include "ns3/node.h"
#include <fstream>
#include <vector>

#include "monitor-app.h"

namespace ns3 {
namespace ndn {

class CC
{
public:
    CC();
    ~CC();

    static void report(Ptr<Node> node, CNMRReport report);
    static void setFilename(std::string filename);

private:
    uint32_t pitSize;
    float gamma;

    std::string filename;
    std::ofstream os;

    // The CC saves the last report it has sent to CNMRs to be able to detect when an attack stopped
    std::set<Name> lastReportedPrefixes;
    std::map<uint32_t, Ptr<ndn::fw::MonitorAwareRouting> > monitors;

    typedef std::map<uint32_t, std::vector<std::pair<int, int> > >  NodePairsMap;

    // The interval at which CC information should be printed (seconds) to file for evaluation
    uint32_t intervalPrint;

    double sizeReports;
    double sizeMessagesSent;
    double sizeMessagesReceived;
    uint32_t numMessagesSent;
    uint32_t numMessagesReceived;

    static double getSizeReport(CNMRReport &report);

    void print(void);

    void onTimerPrint(void);

    void checkForAttack(void);

    typedef std::map<uint32_t, std::vector<CNMRReport> > ReportMap;
    ReportMap reportsCurrentPeriod;
    std::map<uint32_t, CNMRReport> lastReports;
};

} // namespace ndn
} // namespace ns3

#endif
