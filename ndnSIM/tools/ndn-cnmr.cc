#include "ns3/core-module.h"
#include "ns3/ndn-app.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/point-to-point-layout-module.h"
#include "ns3/ndnSIM-module.h"
#include "ns3/topology-reader.h"
#include "ns3/config-store.h"
#include "ns3/gtk-config-store.h"
#include "ns3/ndnSIM/utils/tracers/ndn-cs-tracer.h"
#include "ns3/ndnSIM/model/fw/monitor-aware-routing.h"
#include "ns3/ndnSIM/apps/cnmr-flooding-attacker.h"
#include "ns3/ndnSIM/apps/cnmr-client.h"
#include "cnmr/pit-tracer.h"
#include "cnmr/hops-tracer.h"
#include "cnmr/cc.h"

#include <sys/stat.h>
#include <math.h>

#include <boost/algorithm/string.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/filesystem.hpp>

using namespace ns3;

static GlobalValue g_monitorRouters ("MonitorRouters",
        "",
        ns3::StringValue (""),
        ns3::MakeStringChecker ());

static GlobalValue g_ingressRouters("IngressRouters",
        "Comma-seperated list of routers to which clients should be attached (Node: To attach multiple clients to the same node, write the node ID multiple times, e.g. '0,0,1,2').",
        ns3::StringValue (""),
        ns3::MakeStringChecker ());

static GlobalValue g_egressRouters ("EgressRouters",
        "Comma-seperated list of routers to which servers should be attached. Each server gets a random prefix (Node: To attach multiple servers to the same node, write the node ID multiple times, e.g. '0,0,1,2').",
        ns3::StringValue (""),
        ns3::MakeStringChecker ());

static GlobalValue g_attachAttackersTo("AttachAttackersTo",
        "Comma-seperated list of routers to which malicious clients should be attached (Node: To attach multiple attackers to the same node, write the node ID multiple times, e.g. '0,0,1,2').",
        ns3::StringValue (""),
        ns3::MakeStringChecker ());

static GlobalValue g_randomIngressRouters("RandomIngressRouters",
        "Percent of routers that are ingress routers (1 client will be attached to each ingress router).",
        ns3::DoubleValue (0),
        ns3::MakeDoubleChecker<float> (0, 1));

static GlobalValue g_randomEgressRouters("RandomEgressRouters",
        "Percent of routers that are egress routers (1 server with a random prefix will be attached to each egress router).",
        ns3::DoubleValue (0),
        ns3::MakeDoubleChecker<float> (0, 1));

static GlobalValue g_randomAttackers("RandomAttackers",
        "Percent of client nodes that should act malicious",
        ns3::DoubleValue (0),
        ns3::MakeDoubleChecker<float> (0, 1));

static GlobalValue g_prefixes("Prefixes",
        "Comma-seperated string of the possible prefixes of the servers (each prefix must have a leading '/')",
        ns3::StringValue ("/google.com,/yahoo.com,/youtube.com,/fsf.org,/gnu.org,/kernel.org,/facebook.com,/baidu.com,/reddit.com,/soundcloud.com"),
        ns3::MakeStringChecker ());

static GlobalValue g_topoFile ("TopologyFile",
        "",
        ns3::StringValue ("topologies/AS_3257.gtna.txt"),
        ns3::MakeStringChecker ());

static GlobalValue g_cacheSize ("CacheSize",
        "The size of the cache (lru)",
        ns3::UintegerValue (100),
        ns3::MakeUintegerChecker<uint32_t> ());

static GlobalValue g_pitSize ("PITSize",
        "The maximum size of the PIT.",
        ns3::UintegerValue (5000),
        ns3::MakeUintegerChecker<uint32_t> ());

static GlobalValue g_gamma("gamma",
        "",
        ns3::DoubleValue (0.2),
        ns3::MakeDoubleChecker<float> (0, 1));

static GlobalValue g_pitLifetime("PITLifetime",
        "The PIT timeout/lifetime",
        TimeValue (Seconds (2)),
        MakeTimeChecker());

std::vector<std::basic_string<char> > splitGlobalValue(GlobalValue gv)
{
    StringValue sv;
    gv.GetValue(sv);
    std::string val = sv.Get();
    std::vector<std::string> strs;
    boost::split(strs, val, boost::is_any_of(","));
    return strs;
}

bool file_exists(const std::string& name) {
    // Apparently the fastest way to check whether a file exists
    // http://stackoverflow.com/a/12774387/1518357
    struct stat buffer;
    return (stat (name.c_str(), &buffer) == 0);
}

int main (int argc, char *argv[])
{
    // Load config
    Config::SetDefault ("ns3::ConfigStore::Filename", StringValue ("cnmr-config.txt"));
    Config::SetDefault ("ns3::ConfigStore::Mode", StringValue ("Load"));
    ConfigStore inputConfig;
    inputConfig.ConfigureDefaults ();

    // Read optional command-line parameters
    CommandLine cmd;
    cmd.Parse (argc, argv);

    int run = SeedManager::GetRun();
    int seed = SeedManager::GetSeed();
    std::cout << "Initializing run #" << run << " with seed " << seed << std::endl;

    // Read topology from file
    StringValue svTopoFile;
    g_topoFile.GetValue(svTopoFile);
    AnnotatedTopologyReader topologyReader ("", 25);
    topologyReader.SetFileName (svTopoFile.Get());
    topologyReader.Read ();

    PointToPointHelper p2p;

    NodeContainer allRouters = topologyReader.GetNodes();
    size_t numRouters = allRouters.size();

    Ptr<UniformRandomVariable> rng = CreateObject<UniformRandomVariable>();

    /*
     * Create fixed client nodes at ingress routers
     */
    // "a few milliseconds" for ingress-links
    Config::SetDefault ("ns3::PointToPointChannel::Delay", StringValue ("5ms"));

    DoubleValue v_randomAttackers;
    g_randomAttackers.GetValue(v_randomAttackers);
    double percRandomAttackers = v_randomAttackers.Get();

    DoubleValue v_randomIngressRouters;
    g_randomIngressRouters.GetValue(v_randomIngressRouters);
    double percRandomClients = v_randomIngressRouters.Get();

    std::vector<std::string> strsIngress = splitGlobalValue(g_ingressRouters);
    NodeContainer clientNodes;
    size_t numFixedClients = strsIngress.size();

    std::set<int> alreadyPickedIngress;

    if(numFixedClients > 0 && strsIngress[0] != "")
    {
        if(percRandomAttackers > 0 && percRandomClients > 0)
        {
            std::cout << "Cannot define fixed clients, random clients and random attackers at the same time (choose two of the three)." << std::endl;
            return 1;
        }

        if(percRandomAttackers > 0)
        {
            numFixedClients = ceil(numFixedClients * (1 - percRandomAttackers));

            std::cout << "Creating " << numFixedClients << " fixed client node(s)." << std::endl;
            clientNodes.Create(numFixedClients);

            if(percRandomAttackers > 0)
            {
                for(size_t i_client = 0; i_client < numFixedClients; i_client++)
                {
                    int randomIndex = rng->GetInteger(0, strsIngress.size() - 1);
                    while(alreadyPickedIngress.find(randomIndex) != alreadyPickedIngress.end())
                    {
                        randomIndex = rng->GetInteger(0, strsIngress.size() - 1);
                    }
                    alreadyPickedIngress.insert(randomIndex);

                    const Ptr<Node> client = clientNodes.Get(i_client);
                    const Ptr<Node> ingressRouter = Names::Find<Node>(strsIngress[randomIndex]);
                    Names::Add("client" + boost::lexical_cast<std::string>(i_client), client);

                    p2p.Install (client, ingressRouter);
                    p2p.Install (ingressRouter, client);
                }

            }

        }
        else
        {
            std::cout << "Creating " << numFixedClients << " fixed client node(s)." << std::endl;
            clientNodes.Create(numFixedClients);

            int i_client = 0;
            for (size_t i_ingress = 0; i_ingress < numFixedClients; i_ingress++)
            {
                const Ptr<Node> client = clientNodes.Get(i_client);
                const Ptr<Node> ingressRouter = Names::Find<Node>(strsIngress[i_ingress]);
                Names::Add("client" + boost::lexical_cast<std::string>(i_client), client);

                i_client++;

                p2p.Install (client, ingressRouter);
                p2p.Install (ingressRouter, client);
            }
        }
    }
    else
    {
        numFixedClients = 0;
    }

    /*
     * Create random clients
     */
    if(percRandomClients > 0)
    {
        size_t numRandomClients = ceil(percRandomClients * (1 - percRandomAttackers) * numRouters);
        std::cout << "Creating " << numRandomClients << " random client node(s)." << std::endl;
        clientNodes.Create(numRandomClients);

        std::set<int> alreadyPicked;

        for(size_t i_client = numFixedClients; i_client < clientNodes.size(); i_client++)
        {
            int randomIndex = rng->GetInteger(0, numRouters - 1);
            while(alreadyPicked.find(randomIndex) != alreadyPicked.end())
            {
                randomIndex = rng->GetInteger(0, numRouters - 1);
            }
            alreadyPicked.insert(randomIndex);
            const Ptr<Node> client = clientNodes.Get(i_client);
            const Ptr<Node> ingressRouter = allRouters.Get(randomIndex);

            Names::Add("client" + boost::lexical_cast<std::string>(i_client), client);

            p2p.Install (client, ingressRouter);
            p2p.Install (ingressRouter, client);
        }
    }

    /*
     * Create attacker nodes at ingress routers
     */
    std::vector<std::string> strs = splitGlobalValue(g_attachAttackersTo);
    NodeContainer attackerNodes;
    size_t numFixedAttackers = strs.size();
    if(numFixedAttackers > 0 && strs[0] != "")
    {
        std::cout << "Creating " << numFixedAttackers << " fixed attacker node(s)." << std::endl;
        attackerNodes.Create(numFixedAttackers);

        int i_attacker = 0;
        for (size_t i_ingress = 0; i_ingress < numFixedAttackers; i_ingress++)
        {
            const Ptr<Node> attacker = attackerNodes.Get(i_attacker);
            const Ptr<Node> ingressRouter = Names::Find<Node>(strs[i_ingress]);

            Names::Add("attacker" + boost::lexical_cast<std::string>(i_attacker), attacker);

            i_attacker++;

            p2p.Install (attacker, ingressRouter);
            p2p.Install (ingressRouter, attacker);
        }
    }
    else {
        numFixedAttackers = 0;
    }

    /*
     * Create random attackers
     */
    if(percRandomAttackers > 0)
    {

        size_t numRandomAttackers;
        if(numFixedClients > 0)
        {
            numRandomAttackers = ceil(strsIngress.size() * percRandomAttackers);

            std::cout << "Creating " << numRandomAttackers << " random attacker node(s)." << std::endl;
            attackerNodes.Create(numRandomAttackers);

            size_t i_attacker = 0;

            for(size_t i_ingress = 0; i_ingress < strsIngress.size(); i_ingress++)
            {
                if(alreadyPickedIngress.find(i_ingress) != alreadyPickedIngress.end())
                {
                    continue;
                }

                alreadyPickedIngress.insert(i_ingress);

                const Ptr<Node> attacker = attackerNodes.Get(i_attacker);
                const Ptr<Node> ingressRouter = Names::Find<Node>(strsIngress[i_ingress]);
                Names::Add("attacker" + boost::lexical_cast<std::string>(i_attacker), attacker);

                p2p.Install (attacker, ingressRouter);
                p2p.Install (ingressRouter, attacker);

                i_attacker++;
            }

        }
        else
        {

            numRandomAttackers = ceil(percRandomClients * percRandomAttackers * numRouters);

            std::cout << "Creating " << numRandomAttackers << " random attacker node(s)." << std::endl;
            attackerNodes.Create(numRandomAttackers);

            std::set<int> alreadyPicked;

            for(size_t i_attacker = numFixedAttackers; i_attacker < attackerNodes.size(); i_attacker++)
            {
                int randomIndex = rng->GetInteger(0, numRouters - 1);
                while(alreadyPicked.find(randomIndex) != alreadyPicked.end())
                {
                    randomIndex = rng->GetInteger(0, numRouters - 1);
                }
                alreadyPicked.insert(randomIndex);
                const Ptr<Node> attacker = attackerNodes.Get(i_attacker);
                const Ptr<Node> ingressRouter = allRouters.Get(randomIndex);

                Names::Add("attacker" + boost::lexical_cast<std::string>(i_attacker), attacker);

                p2p.Install (attacker, ingressRouter);
                p2p.Install (ingressRouter, attacker);
            }

        }

    }

    /*
     * Create fixed server nodes a egress routers
     */
    // "a few hundred milliseconds" for ingress-links
    Config::SetDefault ("ns3::PointToPointChannel::Delay", StringValue ("500ms"));

    NodeContainer serverNodes;
    strs = splitGlobalValue(g_egressRouters);
    size_t numFixedServers = strs.size();
    if(numFixedServers > 0 && strs[0] != "")
    {
        // Create as many server nodes as there are egress routers
        std::cout << "Creating " << numFixedServers << " server node(s)." << std::endl;
        serverNodes.Create(numFixedServers);
        for (size_t i = 0; i < numFixedServers; i++)
        {
            const Ptr<Node> egressRouter = Names::Find<Node> (strs[i]);
            const Ptr<Node> server = serverNodes.Get(i);
            Names::Add("server" + boost::lexical_cast<std::string>(i), server);
            p2p.Install (server, egressRouter);
            p2p.Install (egressRouter, server);
        }
    }
    else
    {
        numFixedServers = 0;
    }

    /*
     * Create random servers
     */
    DoubleValue v_randomEgressRouters;
    g_randomEgressRouters.GetValue(v_randomEgressRouters);
    double percRandomServers = v_randomEgressRouters.Get();
    if(percRandomServers > 0)
    {
        size_t numRandomServers = ceil(percRandomServers * numRouters);
        std::cout << "Creating " << numRandomServers << " random server node(s)." << std::endl;
        serverNodes.Create(numRandomServers);

        std::set<int> alreadyPicked;

        for(size_t i_server = numFixedServers; i_server < serverNodes.size(); i_server++)
        {
            int randomIndex = rng->GetInteger(0, numRouters - 1);
            while(alreadyPicked.find(randomIndex) != alreadyPicked.end())
            {
                randomIndex = rng->GetInteger(0, numRouters - 1);
            }
            alreadyPicked.insert(randomIndex);
            const Ptr<Node> server = serverNodes.Get(i_server);
            const Ptr<Node> egressRouter = allRouters.Get(randomIndex);

            Names::Add("server" + boost::lexical_cast<std::string>(i_server), server);

            p2p.Install (server, egressRouter);
            p2p.Install (egressRouter, server);
        }
    }

    if(serverNodes.size() <= 0)
    {
        std::cout << "No servers have been configured. Specify some servers in the config file." << std::endl;
        return 1;
    }

    StringValue v_cacheSize;
    g_cacheSize.GetValue(v_cacheSize);
    std::string cacheSize = v_cacheSize.Get();

    UintegerValue v_pitSize;
    g_pitSize.GetValue(v_pitSize);
    uint32_t pitSize = v_pitSize.Get();

    DoubleValue v_gamma;
    g_gamma.GetValue(v_gamma);
    double gamma = v_gamma.Get();

    TimeValue v_pitLifetime;
    g_pitLifetime.GetValue(v_pitLifetime);

    // Install NDN stack on all nodes
    ndn::StackHelper ndnHelper;
    ndnHelper.SetForwardingStrategy("ns3::ndn::fw::MonitorAwareRouting");
    ndnHelper.SetPit("ns3::ndn::pit::Persistent", "MaxSize", boost::lexical_cast<std::string>(pitSize)); // ns3::ndn::pit::Random ns3::ndn::pit::Lru
    if(cacheSize == "0")
        ndnHelper.SetContentStore("ns3::ndn::cs::Nocache");
    else
        ndnHelper.SetContentStore("ns3::ndn::cs::Lfu", "MaxSize", cacheSize);

    ndnHelper.Install(allRouters);
    ndnHelper.Install(clientNodes);
    ndnHelper.Install(serverNodes);

    // No PIT size limit on attacker nodes
    ndnHelper.SetPit("ns3::ndn::pit::Persistent", "MaxSize", "0"); // ns3::ndn::pit::Random ns3::ndn::pit::Lru
    ndnHelper.Install(attackerNodes);

    // Installing global routing interface on all nodes
    ndn::GlobalRoutingHelper ndnGlobalRoutingHelper;
    ndnGlobalRoutingHelper.InstallAll ();

    /*
     * Configure prefixes
     */
    std::vector<std::string> prefixes = splitGlobalValue(g_prefixes);
    std::ostringstream assignedPrefixes;

    if(prefixes.size() < serverNodes.size())
    {
        std::cout << "Less possible prefixes than server nodes. Please define more prefixes." << std::endl;
        return 1;
    }

    // The prefix that will be "attacked"
    std::string attacktedPrefix = "";

    /*
    * Servers
    */
    ndn::AppHelper serverHelper ("ns3::ndn::CnmrServer");
    for(size_t i_server = 0; i_server < serverNodes.size(); i_server++)
    {
        Ptr<Node> server = serverNodes.Get(i_server);
        int randomIndex = rng->GetInteger(0, prefixes.size() - 1);
        std::string prefix = prefixes[randomIndex];
        serverHelper.SetPrefix (prefix);
        serverHelper.Install (server);

        // To ensure that every server has another prefix
        prefixes.erase(prefixes.begin() + randomIndex);

        ndnGlobalRoutingHelper.AddOrigins (prefix, server);

        if(attacktedPrefix == "")
        {
            // The first randomly chosen prefix will be attacked
            attacktedPrefix = prefix;
        }

        if(i_server == serverNodes.size() - 1)
            assignedPrefixes << prefix;
        else
            assignedPrefixes << prefix << ",";
    }

    /*
     * Install client app
     */
    ndn::AppHelper clientHelper ("ns3::ndn::CnmrClient");
    clientHelper.SetAttribute("LifeTime", v_pitLifetime);
    for(size_t i_client = 0; i_client < clientNodes.size(); i_client++)
    {
        Ptr<Node> client = clientNodes.Get(i_client);
        clientHelper.SetAttribute("Prefixes", StringValue(assignedPrefixes.str()));
        clientHelper.Install (client);
    }

    /*
    * Attacker
    */
    ndn::AppHelper attackerHelper ("ns3::ndn::CnmrFloodingAttacker");
    attackerHelper.SetAttribute("LifeTime", v_pitLifetime);
    std::cout << "Attackers will be attacking: " << attacktedPrefix << std::endl;
    attackerHelper.SetPrefix (attacktedPrefix);
    attackerHelper.Install (attackerNodes);

    /*
    * Monitors
    */
    strs = splitGlobalValue(g_monitorRouters);
    NodeContainer monitorRouters;
    size_t numMonitors = strs.size();
    if(numMonitors > 0 && strs[0] != "") // A simulation could be done without monitors
    {
        std::cout << "Installing MonitorApp on " << numMonitors << " node(s)." << std::endl;
        for (size_t i = 0; i < numMonitors; i++)
        {
            Ptr<Node> monitor = Names::Find<Node> (strs[i]);
            monitorRouters.Add(monitor);

            Names::Rename(strs[i], "monitor" + boost::lexical_cast<std::string>(i));

            // IF MAR2 - TODO: does this hurt if !MAR2????
            ndnGlobalRoutingHelper.AddOrigins ("/monitor/" + strs[i], monitor);
            // END IF
        }

        ndn::AppHelper monitorHelper ("MonitorApp");
        monitorHelper.Install(monitorRouters);

        // Add prefix for closest monitor node
        ndnGlobalRoutingHelper.AddOrigins ("/monitor/", monitorRouters);
    }
    else
    {
        numMonitors = 0;
    }

    NodeContainer normalRouters;
    for(NodeContainer::const_iterator it = allRouters.begin(); it != allRouters.End(); ++it)
    {
        if(Names::FindPath(*it).find("monitor") == std::string::npos)
        {
            normalRouters.Add(*it);
        }
    }

    ndn::AppHelper routerHelper ("RouterApp");
    routerHelper.Install(normalRouters);

    // Fetch observation period
    Time observationPeriod;
    TimeValue vObsPeriod;
    if(monitorRouters.size() > 0)
    {
        Ptr<Application> monitor = monitorRouters.Get(0)->GetApplication(0);
        monitor->GetAttribute("ObservationPeriod", vObsPeriod);
    }
    else
    {
        Ptr<Application> router = normalRouters.Get(0)->GetApplication(0);
        router->GetAttribute("ObservationPeriod", vObsPeriod);
    }
    observationPeriod = vObsPeriod.Get();

    // Calculate and install FIBs
    ndnGlobalRoutingHelper.CalculateRoutes();

    DoubleValue attackerFreqValue;
    double attackerFreq = 0;
    if(attackerNodes.size() > 0)
    {
        // Fetch the frequency of attacker nodes here, to give proper names to the log-files
        attackerNodes.Get(0)->GetApplication(0)->GetAttribute("Frequency", attackerFreqValue);;
        attackerFreq = attackerFreqValue.Get();
    }

    // Fetch the frequency of client nodes here, to give proper names to the log-files
    DoubleValue clientFreqValue;
    clientNodes.Get(0)->GetApplication(0)->GetAttribute("Frequency", clientFreqValue);;
    double clientFreq = clientFreqValue.Get();

    // Fetch payload size of data packages
    UintegerValue vPayloadSize;
    serverNodes.Get(0)->GetApplication(0)->GetAttribute("PayloadSize", vPayloadSize);;
    uint32_t payloadSize = vPayloadSize.Get();

    // Fetch FTBM and MAR
    Ptr<ndn::fw::MonitorAwareRouting> mar = clientNodes.Get(0)->GetObject<ndn::fw::MonitorAwareRouting>();
    EnumValue vMode;
    mar->GetAttribute("Mode", vMode);
    uint32_t marMode = vMode.Get();

    DoubleValue vTau;
    mar->GetAttribute("tau", vTau);
    double tau = vTau.Get();

    BooleanValue vFtbm;
    mar->GetAttribute("FTBM", vFtbm);
    bool ftbm = vFtbm.Get();

    UintegerValue vDetection;
    mar->GetAttribute("Detection", vDetection);
    uint32_t detection = vDetection.Get();

    if((marMode > 0 || ftbm == true) && monitorRouters.size() <= 0)
    {
        std::cout << "MAR/FTBM is enabled but there are not CNMRs." << std::endl;
        return 1;
    }

    if((monitorRouters.size() > 0 || marMode > 0 || ftbm == true) && detection >= 4)
    {
        std::cout << "SBA/SBP should not be used with monitor nodes and/or MAR/FTBM enabled." << std::endl;
        return 1;
    }

    std::string outputDir = "output";
    boost::filesystem::path dir(outputDir);
    if(boost::filesystem::create_directory(dir)) {
        std::cout << "Created output directory: " << outputDir << "\n";
    }

    std::ostringstream simulationName;
    simulationName << "topo=" << svTopoFile.Get().substr(svTopoFile.Get().rfind("/") + 1)
        << "_mar=" << marMode
        << "_ftbm=" << ftbm
        << "_detection=" << detection
        << "_numServers=" << serverNodes.size()
        << "_payloadSize=" << payloadSize
        << "_numClients=" << clientNodes.size() << "@" << clientFreq
        << "_numAttackers=" << attackerNodes.size() << "@" << attackerFreq
        << "_numMonitors=" << numMonitors
        << "_tau=" << tau
        << "_observationPeriod=" << observationPeriod.GetSeconds() << "s"
        << "_gamma=" << gamma
        << "_cacheSize=" << cacheSize
        << "_pitSize=" << pitSize
        << "_pitLifetime=" << v_pitLifetime.Get().GetSeconds() << "s";

    std::ostringstream tracerFiles;
    tracerFiles << outputDir << "/" << simulationName.str() << "_run=" << run << "_seed=" << seed;

    std::ostringstream l3TracerFile;
    l3TracerFile << tracerFiles.str() << "-l3trace.txt";
    if(file_exists(l3TracerFile.str()))
    {
        std::cout << l3TracerFile.str() << " exists. It will be overwritten." << std::endl;
    }

    std::ostringstream appDelayTracerFile;
    appDelayTracerFile << tracerFiles.str() << "-appdelays.txt";
    if(file_exists(appDelayTracerFile.str()))
    {
        std::cout << appDelayTracerFile.str() << " exists. It will be overwritten." << std::endl;
    }

    std::ostringstream pitTracerFile;
    pitTracerFile << tracerFiles.str() << "-pit.txt";
    if(file_exists(pitTracerFile.str()))
    {
        std::cout << pitTracerFile.str() << " exists. It will be overwritten." << std::endl;
    }

    std::ostringstream hopsTracerFile;
    hopsTracerFile << tracerFiles.str() << "-hops.txt";
    if(file_exists(hopsTracerFile.str()))
    {
        std::cout << hopsTracerFile.str() << " exists. It will be overwritten." << std::endl;
    }

    std::ostringstream ccFile;
    ccFile << tracerFiles.str() << "-cc.txt";
    if(file_exists(ccFile.str()))
    {
        std::cout << ccFile.str() << " exists. It will be overwritten." << std::endl;
    }
    ns3::ndn::CC::setFilename(ccFile.str());

    NodeContainer l3tracedNodes;
    l3tracedNodes.Add(serverNodes);
    l3tracedNodes.Add(clientNodes);
    l3tracedNodes.Add(attackerNodes);
    l3tracedNodes.Add(monitorRouters);
    ndn::L3AggregateTracer::Install(l3tracedNodes, l3TracerFile.str(), Seconds(10.0));

    NodeContainer consumingNodes;
    consumingNodes.Add(allRouters);
    consumingNodes.Add(serverNodes);

    if(marMode > 0)
        ndn::HopsTracer::Install(consumingNodes, hopsTracerFile.str());

    ndn::PitTracer::Install(allRouters, pitTracerFile.str(), Seconds(10.0));

    Simulator::Stop (Minutes (9.0));

    std::cout << "Starting run #" << run << " of simulation: " << simulationName.str() << " with seed " << seed << std::endl;
    ndn::CsTracer::InstallAll("rate-trace.txt", Seconds(0.5));
    Simulator::Run ();
    Simulator::Destroy ();

    return 0;
}
