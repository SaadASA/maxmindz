#include "cc.h"
#include "ns3/core-module.h"
#include "ns3/log.h"
#include "ns3/simulation-singleton.h"

#include <boost/foreach.hpp>

NS_LOG_COMPONENT_DEFINE ("CC");

namespace ns3 {
namespace ndn {

using namespace fw;

static GlobalValue g_gamma("gamma",
        "",
        ns3::DoubleValue (0.2),
        ns3::MakeDoubleChecker<float> (0, 1));

CC::CC()
{
    // The interval at which the CC prints stats to the evaluation file
    intervalPrint = 10;
    Simulator::Schedule(Seconds(intervalPrint), &CC::onTimerPrint, this);

    UintegerValue v_pitSize;
    GlobalValue::GetValueByName("PITSize", v_pitSize);
    pitSize = v_pitSize.Get();

    DoubleValue v_gamma;
    g_gamma.GetValue(v_gamma);
    gamma = v_gamma.Get();
}

CC::~CC()
{
    os.close();
}

void CC::report(Ptr<Node> node, CNMRReport report)
{
    // Get the singleton of the CC
    CC *cc = SimulationSingleton<CC>::Get();

    // Count the number of control messages received
    cc->numMessagesReceived++;

    uint32_t nodeId = node->GetId();

    if(cc->monitors[nodeId] == 0)
    {
        // Save a reference to every monitor that reports to the CC, so that the CC can report back
        // to those monitors
        cc->monitors[nodeId] = node->GetObject<ndn::fw::MonitorAwareRouting>();
    }

    // Just save the report
    cc->reportsCurrentPeriod[nodeId].push_back(report);
    cc->lastReports[nodeId] = report;

    // Calculate the size of the report
    double sizeMsg = cc->getSizeReport(report);
    cc->sizeReports += sizeMsg;
    cc->sizeMessagesReceived += sizeMsg;
    cc->numMessagesReceived++;

    cc->checkForAttack();
}

void CC::checkForAttack(void)
{
    std::set<Name> maliciousPrefixes;

    std::map<Name, std::vector<int> > timedOutEntriesPerName;

    // Aggregate reported PIT usages of nodes and check if the usage of one prefix is above threshold
    for(std::map<uint32_t, CNMRReport>::const_iterator it = lastReports.begin(); it != lastReports.end(); ++it)
    {
        CNMRReport report = it->second;
        for(MonitorAwareRouting::PerNameCounter::iterator count = report.timedOutEntriesPerName.begin(); count != report.timedOutEntriesPerName.end(); ++count)
        {
            timedOutEntriesPerName[count->first].push_back(count->second);
        }
    }

    std::pair<Name, std::vector<int> > d;
    BOOST_FOREACH(d, timedOutEntriesPerName)
    {
        uint32_t numTimedOut = 0;
        BOOST_FOREACH(int i, d.second)
        {
            numTimedOut += i;
        }

        float expireRatio = (float)numTimedOut / pitSize;

        if(expireRatio >= gamma)
        {
            maliciousPrefixes.insert(d.first);
        }
    }

    // If the determined malicious prefix are different from the prefixes that the CC reported as
    // malicious the last time, report it!
    if(maliciousPrefixes != lastReportedPrefixes)
    {
        // the name for routing the control message, e.g. /cc/control_message/
        double sizeMsg = sizeof(Name) + sizeof(uint32_t) + (maliciousPrefixes.size() * sizeof(Name));
        sizeReports += sizeMsg;
        numMessagesSent++;
        sizeMessagesSent += sizeMsg;

        lastReportedPrefixes = maliciousPrefixes;

        // Report to CNMRs
        for(std::map<uint32_t, Ptr<ndn::fw::MonitorAwareRouting> >::const_iterator it = monitors.begin(); it != monitors.end(); ++it)
        {
            it->second->setMaliciousPrefixes(maliciousPrefixes);
        }

    }
}

void CC::setFilename(std::string filename)
{
    SimulationSingleton<CC>::Get()->filename = filename;
    SimulationSingleton<CC>::Get()->os.open(filename.c_str());

    if (!SimulationSingleton<CC>::Get()->os.is_open ())
    {
        std::cout << "Could not open file for CC trace. Not writing..." << std::endl;
    }
    else
    {
        // Print header to file
        SimulationSingleton<CC>::Get()->os << "Time\tNode\tFace\tSignal\tValue\n";
    }
}

/**
 * Print to evaluation file
 */
void CC::print(void)
{
    if(reportsCurrentPeriod.size() == 0)
        // No reports -> don't print to file
        return;

    Time time = Simulator::Now ();

    // os << time.ToDouble(Time::S) << "\tCC\tall\t" << "Overhead\t" << sizeReports << "\n";
    os << time.ToDouble(Time::S) << "\tCC\tall\t" << "NumReceived\t" << numMessagesReceived << "\n";
    os << time.ToDouble(Time::S) << "\tCC\tall\t" << "NumSent\t" << numMessagesSent << "\n";
    os << time.ToDouble(Time::S) << "\tCC\tall\t" << "SizeReceived\t" << sizeMessagesReceived << "\n";
    os << time.ToDouble(Time::S) << "\tCC\tall\t" << "SizeSent\t" << sizeMessagesSent << "\n";

    os.flush();

    // Reset stats
    reportsCurrentPeriod.clear();
    sizeReports = 0;
    numMessagesSent = 0;
    numMessagesReceived = 0;
    sizeMessagesSent = 0;
    sizeMessagesReceived = 0;
}

void CC::onTimerPrint(void)
{
    // Print to file
    SimulationSingleton<CC>::Get()->print();
    Simulator::Schedule(Seconds(intervalPrint), &CC::onTimerPrint, this);
}

/**
 * Returns the size of the report if it would have been transmitted over the wire.
 */
double CC::getSizeReport(CNMRReport &report)
{
    // Important: keep this up to date

    double size = 0;

    // the name for routing the control message, e.g. /cc/control_message/
    size += 1 * sizeof(Name);

    // the name for routing the control message, e.g. /cc/control_message/
    size += 1 * sizeof(uint32_t);

    size += report.timedOutEntriesPerName.size() * (sizeof(Name) + sizeof(uint32_t));

    return size;
}

} // namespace ndn
} // namespace ns3
