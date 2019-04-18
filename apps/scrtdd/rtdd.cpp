/***************************************************************************
 *   Copyright (C) by ETHZ/SED                                             *
 *                                                                         *
 *   This program is free software: you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation, either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   Developed by Luca Scarabello <luca.scarabello@sed.ethz.ch>            *
 ***************************************************************************/


#include "rtdd.h"
#include "csvreader.h"

#include <seiscomp3/logging/filerotator.h>
#include <seiscomp3/logging/channel.h>

#include <seiscomp3/core/strings.h>
#include <seiscomp3/core/genericrecord.h>
#include <seiscomp3/core/system.h>

#include <seiscomp3/client/inventory.h>
#include <seiscomp3/io/archive/xmlarchive.h>
#include <seiscomp3/io/records/mseedrecord.h>

#include <seiscomp3/datamodel/event.h>
#include <seiscomp3/datamodel/pick.h>
#include <seiscomp3/datamodel/origin.h>
#include <seiscomp3/datamodel/magnitude.h>
#include <seiscomp3/datamodel/utils.h>
#include <seiscomp3/datamodel/parameter.h>
#include <seiscomp3/datamodel/parameterset.h>
#include <seiscomp3/datamodel/journalentry.h>
#include <seiscomp3/datamodel/utils.h>

#include <seiscomp3/math/geo.h>
#include <seiscomp3/math/filter/butterworth.h>

#include <seiscomp3/utils/files.h>

#include <boost/bind.hpp>
#include <boost/filesystem.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>

using namespace std;
using namespace Seiscomp::Processing;
using namespace Seiscomp::DataModel;
using Seiscomp::Core::stringify;


#define JOURNAL_ACTION           "RTDD"
#define JOURNAL_ACTION_COMPLETED "completed"


namespace Seiscomp {

#define NEW_OPT(var, ...) addOption(&var, __VA_ARGS__)
#define NEW_OPT_CLI(var, ...) addOption(&var, nullptr, __VA_ARGS__)


namespace {

using Seiscomp::Core::fromString;

// Rectangular region class defining a rectangular region
// by latmin, lonmin, latmax, lonmax.
struct RectangularRegion : public Seiscomp::RTDD::Region
{
    RectangularRegion() {
    }

    bool init(const Application* app, const string &prefix) {
        vector<string> region;
        try { region = app->configGetStrings(prefix + "region"); }
        catch ( ... ) {}

        if ( region.empty() )
            isEmpty = true;
        else {
            isEmpty = false;

            // Parse region
            if ( region.size() != 4 ) {
                SEISCOMP_ERROR("%s: expected 4 values in region definition, got %d",
                               prefix.c_str(), (int)region.size());
                return false;
            }

            if ( !fromString(latMin, region[0]) ||
                 !fromString(lonMin, region[1]) ||
                 !fromString(latMax, region[2]) ||
                 !fromString(lonMax, region[3]) ) {
                SEISCOMP_ERROR("%s: invalid region value(s)", prefix.c_str());
                return false;
            }
        }

        return true;
    }

    bool isInside(double lat, double lon) const {
        if ( isEmpty ) return true;

        double len, dist;

        if ( lat < latMin || lat > latMax ) return false;

        len = lonMax - lonMin;
        if ( len < 0 )
            len += 360.0;

        dist = lon - lonMin;
        if ( dist < 0 )
            dist += 360.0;

        return dist <= len;
    }

    bool isEmpty;
    double latMin, lonMin;
    double latMax, lonMax;
};

// Rectangular region class defining a circular region
// by lat, lon, radius.
struct CircularRegion : public Seiscomp::RTDD::Region
{
    CircularRegion() {
    }

    bool init(const Application* app, const string &prefix) {
        vector<string> region;
        try { region = app->configGetStrings(prefix + "region"); }
        catch ( ... ) {}

        if ( region.empty() )
            isEmpty = true;
        else {
            isEmpty = false;

            // Parse region
            if ( region.size() != 3 ) {
                SEISCOMP_ERROR("%s: expected 3 values in region definition, got %d",
                               prefix.c_str(), (int)region.size());
                return false;
            }

            if ( !fromString(lat, region[0]) ||
                 !fromString(lon, region[1]) ||
                 !fromString(radius, region[2]) ) {
                SEISCOMP_ERROR("%s: invalid region value(s)", prefix.c_str());
                return false;
            }
        }

        return true;
    }

    bool isInside(double lat, double lon) const {
        if ( isEmpty ) return true;

        double distance, az, baz;
        Math::Geo::delazi(this->lat, this->lon, lat, lon, &distance, &az, &baz);
        double distKm = Math::Geo::deg2km(distance);
        return distKm <= radius;
    }

    bool isEmpty;
    double lat, lon, radius;
};


Core::Time now; // this is tricky, I don't like it


void makeUpper(string &dest, const string &src)
{
    dest = src;
    for ( size_t i = 0; i < src.size(); ++i )
        dest[i] = toupper(src[i]);
}


bool startsWith(const string& haystack, const string& needle, bool caseSensitive = true)
{
    string _haystack = haystack;
    string _needle   = needle;
    if ( !caseSensitive )
    {
        makeUpper(_haystack, haystack);
        makeUpper(_needle, needle);
    }
    return _haystack.compare(0, _needle.length(), _needle) == 0;
}

} // unnamed namespace


RTDD::Config::Config()
{
    publicIDPattern = "RTDD.@time/%Y%m%d%H%M%S.%f@.@id@";
    workingDirectory = "/tmp/rtdd";
    keepWorkingFiles = false;
    onlyPreferredOrigin = false;
    processManualOrigin = true;
    profileTimeAlive = -1;
    cacheWaveforms = false;

    forceProcessing = false;
    testMode = false;
    fExpiry = 1.0;

    wakeupInterval = 10;
    logCrontab = true;
}



RTDD::RTDD(int argc, char **argv) : Application(argc, argv)
{
    setAutoApplyNotifierEnabled(true);
    setInterpretNotifierEnabled(true);

    setLoadInventoryEnabled(true);
    setLoadConfigModuleEnabled(true);

    setPrimaryMessagingGroup("LOCATION");

    addMessagingSubscription("EVENT");
    addMessagingSubscription("LOCATION");
    addMessagingSubscription("PICK"); // this is only for caching picks

    setAutoAcquisitionStart(false);
    setAutoCloseOnAcquisitionFinished(false);

    _cache.setPopCallback(boost::bind(&RTDD::removedFromCache, this, _1));

    _processingInfoChannel = nullptr;
    _processingInfoOutput = nullptr;

    NEW_OPT(_config.publicIDPattern, "publicIDpattern");
    NEW_OPT(_config.workingDirectory, "workingDirectory");
    NEW_OPT(_config.keepWorkingFiles, "keepWorkingFiles");
    NEW_OPT(_config.onlyPreferredOrigin, "onlyPreferredOrigin");
    NEW_OPT(_config.processManualOrigin, "manualOrigin");
    NEW_OPT(_config.activeProfiles, "activeProfiles");

    NEW_OPT(_config.wakeupInterval, "cron.wakeupInterval");
    NEW_OPT(_config.logCrontab, "cron.logging");
    NEW_OPT(_config.delayTimes, "cron.delayTimes");

    NEW_OPT(_config.profileTimeAlive, "performance.profileTimeAlive");
    NEW_OPT(_config.cacheWaveforms, "performance.cacheWaveforms");

    NEW_OPT_CLI(_config.testMode, "Mode", "test", "Test mode, no messages are sent", false, true);
    NEW_OPT_CLI(_config.forceProcessing, "Mode", "force",
                "Force event processing: in single event mode the processing is performed even on origins that would normally be skipped (already processed, not preferred, manual, etc.); In catalog mode the processing overwrites any previous processing files, which could be still on disk if 'keepWorkingFiles' option is used",
                false, true);
    NEW_OPT_CLI(_config.fExpiry, "Mode", "expiry,x",
                "Time span in hours after which objects expire", true);
    NEW_OPT_CLI(_config.originIDs, "Mode", "origin-id,O",
                "Relocate the origin (or multiple comma-separated origins) and send a message ", true);
    NEW_OPT_CLI(_config.eventXML, "Mode", "ep",
                "Event parameters XML file for offline processing of contained origins (imply test option). Ech origin will be processed accordingly with the matching profile configuration", true);
    NEW_OPT_CLI(_config.forceProfile, "Mode", "profile",
                "Force a specific profile to be used", true);
    NEW_OPT_CLI(_config.relocateCatalog, "Mode", "reloc-catalog",
                "Relocate the catalog of profile passed as argument", true);
    NEW_OPT_CLI(_config.dumpCatalog, "Mode", "dump-catalog",
                "Dump catalog files from the seiscomp event/origin ids file passed as argument", true);
    NEW_OPT_CLI(_config.loadCatalog, "Mode", "load-catalog",
                "Load catalog waveforms and save them in the working directory configured for the profile", true);
}



RTDD::~RTDD() {
}


void RTDD::createCommandLineDescription() {
    Application::createCommandLineDescription();
    commandline().addOption("Mode", "dump-config", "Dump the configuration and exit");  
    commandline().addOption<string>("Mode", "ph2dt-path", "Specify path to ph2dt executable", nullptr, false);
    commandline().addOption<string>("Mode", "use-ph2dt",
                            "When relocating a catalog use ph2dt. This option requires a ph2dt control file", nullptr, false);
}



bool RTDD::validateParameters() 
{
    Environment *env = Environment::Instance();

    if ( !Application::validateParameters() )
        return false;

    // Disable messaging (offline mode) with:
    //  --ep option
    //  --dump-catalog option
    //  --relocate-catalog option
    //  --O and --test (relocate origin and don't send the new one)
    if ( !_config.eventXML.empty()        ||
         !_config.dumpCatalog.empty()     ||
         !_config.loadCatalog.empty()     ||
         !_config.relocateCatalog.empty() ||
         (!_config.originIDs.empty() && _config.testMode)
       )
    {
        setMessagingEnabled(false);
        _config.testMode = true; // we won't send any message
    }

    std::string hypoddExec = "hypodd";
    try {
        hypoddExec = env->absolutePath(configGetPath("hypoddPath"));
    } catch ( ... ) { }

    bool profilesOK = true;
    bool profileRequireDB = false;

    for ( vector<string>::iterator it = _config.activeProfiles.begin();
          it != _config.activeProfiles.end(); it++ )
    {

        ProfilePtr prof = new Profile;
        string prefix = string("profile.") + *it + ".";

        prof->name = *it;

        try {
            prof->earthModelID = configGetString(prefix + "earthModelID");
        } catch ( ... ) {}
        try {
            prof->methodID = configGetString(prefix + "methodID");
        } catch ( ... ) {}
        if ( ! startsWith(prof->methodID, "RTDD", false) )
        {
            prof->methodID = "RTDD" + prof->methodID;
        }

        string regionType;
        try {
            makeUpper(regionType, configGetString(prefix + "regionType"));
        } catch ( ... ) {}
        if ( regionType == "RECTANGULAR" )
            prof->region = new RectangularRegion;
        else if ( regionType == "CIRCULAR" )
            prof->region = new CircularRegion;

        if ( prof->region == nullptr ) {
            SEISCOMP_ERROR("profile.%s: invalid region type: %s",
                           it->c_str(), regionType.c_str());
            it = _config.activeProfiles.erase(it);
            profilesOK = false;
            continue;
        }

        if ( !prof->region->init(this, prefix) ) {
            SEISCOMP_ERROR("profile.%s: invalid region parameters", it->c_str());
            it = _config.activeProfiles.erase(it);
            profilesOK = false;
            continue;
        }

        prefix = string("profile.") + *it + ".catalog.";

        string eventFile = env->absolutePath(configGetPath(prefix + "eventFile"));

        // check if the file contains only seiscomp event/origin ids
        if ( CSV::readWithHeader(eventFile)[0].count("seiscompId") != 0)
        {
            prof->eventIDFile = eventFile;
            profileRequireDB = true;
        }
        else
        {
            prof->eventFile = eventFile;
            prof->stationFile = env->absolutePath(configGetPath(prefix + "stationFile"));
            prof->phaFile = env->absolutePath(configGetPath(prefix + "phaFile"));
        }
        try {
            prof->incrementalCatalogFile = env->absolutePath(configGetPath(prefix + "incrementalCatalogFile"));
        } catch ( ... ) { }

        if ( ! prof->incrementalCatalogFile.empty() ) profileRequireDB = true;

        try {
            prof->ddcfg.validPphases = configGetStrings(prefix + "P-Phases");
        } catch ( ... ) {
            prof->ddcfg.validPphases = {"P","Pg","Pn","P1"};
        }
        try {
            prof->ddcfg.validSphases = configGetStrings(prefix + "S-Phases");
        } catch ( ... ) {
            prof->ddcfg.validSphases = {"S","Sg","Sn","S1"};
        }

        prefix = string("profile.") + *it + ".dtct.";
        try {
            prof->ddcfg.dtct.minWeight = configGetDouble(prefix + "minWeight");
        } catch ( ... ) { prof->ddcfg.dtct.minWeight = 0; }
        try {
            prof->ddcfg.dtct.minEStoIEratio = configGetDouble(prefix + "minEStoIEratio");
        } catch ( ... ) { prof->ddcfg.dtct.minEStoIEratio = 0; }
        try {
            prof->ddcfg.dtct.minESdist = configGetDouble(prefix + "minESdist");
        } catch ( ... ) { prof->ddcfg.dtct.minESdist = 0; }
        try {
            prof->ddcfg.dtct.maxESdist = configGetDouble(prefix + "maxESdist");
        } catch ( ... ) { prof->ddcfg.dtct.maxESdist = -1; }
        try {
            prof->ddcfg.dtct.maxIEdist = configGetDouble(prefix + "maxIEdist");
        } catch ( ... ) { prof->ddcfg.dtct.maxIEdist = -1; }
        try {
            prof->ddcfg.dtct.minNumNeigh = configGetInt(prefix + "minNumNeigh");
        } catch ( ... ) { prof->ddcfg.dtct.minNumNeigh = 1; }
        try {
            prof->ddcfg.dtct.maxNumNeigh = configGetInt(prefix + "maxNumNeigh");
        } catch ( ... ) { prof->ddcfg.dtct.maxNumNeigh = -1; }
        try {
            prof->ddcfg.dtct.minDTperEvt = configGetInt(prefix + "minDTperEvt");
        } catch ( ... ) { prof->ddcfg.dtct.minDTperEvt = 1; }

        prefix = string("profile.") + *it + ".dtcc.";
        prof->ddcfg.dtcc.recordStreamURL = recordStreamURL();
        try {
            prof->ddcfg.dtcc.minWeight = configGetDouble(prefix + "minWeight");
        } catch ( ... ) { prof->ddcfg.dtcc.minWeight = 0; }
        try {
            prof->ddcfg.dtcc.minEStoIEratio = configGetDouble(prefix + "minEStoIEratio");
        } catch ( ... ) { prof->ddcfg.dtcc.minEStoIEratio = 0; }
        try {
            prof->ddcfg.dtcc.minESdist = configGetDouble(prefix + "minESdist");
        } catch ( ... ) { prof->ddcfg.dtcc.minESdist = 0; }
        try {
            prof->ddcfg.dtcc.maxESdist = configGetDouble(prefix + "maxESdist");
        } catch ( ... ) { prof->ddcfg.dtcc.maxESdist = -1; }
        try {
            prof->ddcfg.dtcc.maxIEdist = configGetDouble(prefix + "maxIEdist");
        } catch ( ... ) { prof->ddcfg.dtcc.maxIEdist = -1; }
        try {
            prof->ddcfg.dtcc.minNumNeigh = configGetInt(prefix + "minNumNeigh");
        } catch ( ... ) { prof->ddcfg.dtcc.minNumNeigh = 1; }
        try {
            prof->ddcfg.dtcc.maxNumNeigh = configGetInt(prefix + "maxNumNeigh");
        } catch ( ... ) { prof->ddcfg.dtcc.maxNumNeigh = -1; }
        try {
            prof->ddcfg.dtcc.minDTperEvt = configGetInt(prefix + "minDTperEvt");
        } catch ( ... ) { prof->ddcfg.dtcc.minDTperEvt = 1; }

        prefix = string("profile.") + *it + ".dtcc.crosscorrelation.";
        try {
            prof->ddcfg.xcorr.timeBeforePick = configGetDouble(prefix + "timeBeforePick");
            prof->ddcfg.xcorr.timeAfterPick = configGetDouble(prefix + "timeAfterPick");
            prof->ddcfg.xcorr.maxDelay = configGetDouble(prefix + "maxDelay");
            prof->ddcfg.xcorr.minCoef = configGetDouble(prefix + "minCCCoef");
        } catch ( ... ) {
            SEISCOMP_ERROR("profile.%s: invalid or missing cross correlation parameters", it->c_str());
            profilesOK = false;
            continue;
        }

        prefix = string("profile.") + *it + ".dtcc.waveformFiltering.";
        try {
            prof->ddcfg.wfFilter.filterFmin = configGetDouble(prefix + "filterFmin");
        } catch ( ... ) { prof->ddcfg.wfFilter.filterFmin = 0.; }
        try {
            prof->ddcfg.wfFilter.filterFmax = configGetDouble(prefix + "filterFmax");
        } catch ( ... ) { prof->ddcfg.wfFilter.filterFmax = 0.; }
        try {
            prof->ddcfg.wfFilter.filterOrder = configGetInt(prefix + "filterOrder");
        } catch ( ... ) { prof->ddcfg.wfFilter.filterOrder = 3; }
        try {
            prof->ddcfg.wfFilter.resampleFreq = configGetDouble(prefix + "resampling");
        } catch ( ... ) { prof->ddcfg.wfFilter.resampleFreq = 0.; }

        prefix = string("profile.") + *it + ".dtcc.snr.";
        try {
            prof->ddcfg.snr.minSnr = configGetDouble(prefix + "minSnr");
        } catch ( ... ) { prof->ddcfg.snr.minSnr = 0.; }
        try {
            prof->ddcfg.snr.noiseStart = configGetDouble(prefix + "noiseStart");
            prof->ddcfg.snr.noiseEnd = configGetDouble(prefix + "noiseEnd");
            prof->ddcfg.snr.signalStart = configGetDouble(prefix + "signalStart");
            prof->ddcfg.snr.signalEnd = configGetDouble(prefix + "signalEnd");
        } catch ( ... ) {
            if ( prof->ddcfg.snr.minSnr > 0. )
            {
                SEISCOMP_ERROR("profile.%s: invalid or missing snr parameters", it->c_str());
                profilesOK = false;
                continue;
            }
        }

        prefix = string("profile.") + *it + ".hypoDD.";
        prof->ddcfg.hypodd.step1CtrlFile = env->absolutePath(configGetPath(prefix + "step1ControlFile"));
        prof->ddcfg.hypodd.step2CtrlFile = env->absolutePath(configGetPath(prefix + "step2ControlFile"));
        prof->ddcfg.hypodd.exec = hypoddExec;

        if ( commandline().hasOption("ph2dt-path") )
            prof->ddcfg.ph2dt.exec = env->absolutePath(commandline().option<string>("ph2dt-path"));
        if ( commandline().hasOption("use-ph2dt") )
            prof->ddcfg.ph2dt.ctrlFile = env->absolutePath(commandline().option<string>("use-ph2dt"));

        _profiles.push_back(prof);
    }

    // If the inventory is provided by an XML file or an event XML
    // is provided, disable the database because we don't need to access it
    if (  !isInventoryDatabaseEnabled() ||
         ( !_config.eventXML.empty() && ! profileRequireDB ) )
    {
        setDatabaseEnabled(false, false);
    }

    if (!profilesOK) return false;

    if ( commandline().hasOption("dump-config") )
    {
        for ( Options::const_iterator it = options().begin(); it != options().end(); ++it )
        {
            if ( (*it)->cfgName )
                cout << (*it)->cfgName;
            else if ( (*it)->cliParam)
                cout << "--" << (*it)->cliParam;
            else
                continue;

            cout << ": ";
            (*it)->printStorage(cout);
            cout << endl;
        }

        return false;
    }

    return true;
}



bool RTDD::init() {

    if ( !Application::init() )
        return false;

    _config.workingDirectory = boost::filesystem::path(_config.workingDirectory).string();
    if ( !Util::pathExists(_config.workingDirectory) )
    {
        if ( ! Util::createPath(_config.workingDirectory) ) {
            SEISCOMP_ERROR("workingDirectory: failed to create path %s",_config.workingDirectory.c_str());
            return false;
        }
    }

    // Log into processing/info to avoid logging the same information into the global info channel
    _processingInfoChannel = SEISCOMP_DEF_LOGCHANNEL("processing/info", Logging::LL_INFO);
    _processingInfoOutput = new Logging::FileRotatorOutput(Environment::Instance()->logFile("scrtdd-processing-info").c_str(),  60*60*24, 30);

    _processingInfoOutput->subscribe(_processingInfoChannel);

    _inputEvts = addInputObjectLog("event");
    _inputOrgs = addInputObjectLog("origin");
    _outputOrgs = addOutputObjectLog("origin", primaryMessagingGroup());

    _cache.setTimeSpan(Core::TimeSpan(_config.fExpiry*3600.));
    _cache.setDatabaseArchive(query());

    // Enable periodic timer: handleTimeout()
    enableTimer(1);

    // Check each 10 seconds if a new job needs to be started
    _cronCounter = _config.wakeupInterval;

    return true;
}



bool RTDD::run() {

    // load Event parameters XML file into _eventParameters
    if ( !_config.eventXML.empty() )
    {
        IO::XMLArchive ar;
        if ( !ar.open(_config.eventXML.c_str()) ) {
            SEISCOMP_ERROR("Unable to open %s", _config.eventXML.c_str());
            return false;
        }

        ar >> _eventParameters;
        ar.close();

        if ( !_eventParameters ) {
            SEISCOMP_ERROR("No event parameters found in %s", _config.eventXML.c_str());
            return false;
        }
    }

    // dump catalog and exit
    if ( !_config.dumpCatalog.empty() )
    {
        HDD::DataSource dataSrc(query(), &_cache, _eventParameters.get());
        HDD::CatalogPtr cat = new HDD::Catalog();
        cat->add(_config.dumpCatalog, dataSrc);
        cat->writeToFile("event.csv","phase.csv","station.csv");
        return true;
    }

    // load catalog and exit
    if ( !_config.loadCatalog.empty() )
    {
        for (list<ProfilePtr>::iterator it = _profiles.begin(); it != _profiles.end(); ++it )
        {
            ProfilePtr profile = *it;
            if ( profile->name == _config.loadCatalog)
            {
                profile->load(query(), &_cache, _eventParameters.get(),
                              _config.workingDirectory, !_config.keepWorkingFiles,
                              true, true);
                profile->unload();
                break;
            }
        } 
        return true;
    }

    // relocate full catalog and exit
    if ( !_config.relocateCatalog.empty() )
    {
        for (list<ProfilePtr>::iterator it = _profiles.begin(); it != _profiles.end(); ++it )
        {
            ProfilePtr profile = *it;
            if ( profile->name == _config.relocateCatalog)
            {
                profile->load(query(), &_cache, _eventParameters.get(),
                              _config.workingDirectory, !_config.keepWorkingFiles,
                              _config.cacheWaveforms, false);
                HDD::CatalogPtr relocatedCat = profile->relocateCatalog(_config.forceProcessing);
                profile->unload();
                relocatedCat->writeToFile("event.csv","phase.csv","station.csv");
                break;
            }
        }
        return true;
    }
    
    // relocate passed origin and exit
    if ( !_config.originIDs.empty() )
    {
        // force process of any origin
        _config.onlyPreferredOrigin = false;

        // split multiple origiss
        std::vector<std::string> ids;
        boost::split(ids, _config.originIDs, boost::is_any_of(","), boost::token_compress_on);
        for (const string& originID : ids)
        {
            OriginPtr org = _cache.get<Origin>(originID);
            if ( !org ) {
                SEISCOMP_ERROR("Event %s  not found.", originID.c_str());
                continue;
            }

            // Start processing immediately
            _config.delayTimes = {0};
            _cronCounter = 0;
            addProcess(org.get());
        }
        return true;
    }

    // relocate xml event and exit
    if ( !_config.eventXML.empty() )
    {
        vector<OriginPtr> origins;
        for(unsigned i = 0; i < _eventParameters->originCount(); i++)
            origins.push_back(_eventParameters->origin(i));

        for(const OriginPtr& org : origins)
        {
            // Start processing immediately
            _config.delayTimes = {0};
            _cronCounter = 0;

            if ( !addProcess(org.get()) )
                return false;
        }

        IO::XMLArchive ar;
        ar.create("-");
        ar.setFormattedOutput(true);
        ar << _eventParameters;
        ar.close();
        return true;
    }

    // real time processing
    return Application::run();
}



void RTDD::done() {
    Application::done();

    // Remove crontab log file if exists
    unlink((Environment::Instance()->logDir() + "/" + name() + ".sched").c_str());

    if ( _processingInfoChannel ) delete _processingInfoChannel;
    if ( _processingInfoOutput )  delete _processingInfoOutput;
}



void RTDD::handleMessage(Core::Message *msg)
{
    Application::handleMessage(msg);

    // Add all events collected by addObject/updateObject
    Todos::iterator it;
    for ( it = _todos.begin(); it != _todos.end(); ++it )
        addProcess(it->get());
    _todos.clear();
}



void RTDD::addObject(const string& parentID, DataModel::Object* object)
{
    updateObject(parentID, object);
}



void RTDD::updateObject(const string &parentID, Object* object)
{
    Pick *pick = Pick::Cast(object);
    if ( pick )
    {
        _cache.feed(pick);
        return;
    }

    Origin *origin = Origin::Cast(object);
    if ( origin )
    {
        _todos.insert(origin);
        logObject(_inputOrgs, Core::Time::GMT());
        return;
    }

    Event *event = Event::Cast(object);
    if ( event )
    {
        _todos.insert(event);
        logObject(_inputEvts, now);
        return;
    }
}



void RTDD::handleTimeout() 
{
    checkProfileStatus();
    runNewJobs();
}



void RTDD::checkProfileStatus() 
{
    // Periodically clean up profiles unused for some time as they
    // might use lots of memory (waveform data)
    // OR, if the profiles are configured to neer expire, make sure
    // they are loaded

    for (list<ProfilePtr>::iterator it = _profiles.begin(); it != _profiles.end(); ++it )
    {
        ProfilePtr currProfile = *it;
        if (_config.profileTimeAlive < 0) // nevel clean up profiles, force loading
        {
            if ( ! currProfile->isLoaded() )
            {
                currProfile->load(query(), &_cache, _eventParameters.get(),
                                  _config.workingDirectory, !_config.keepWorkingFiles,
                                  _config.cacheWaveforms, true);
            }
        }
        else  // periodic clean up of profiles
        {
            Core::TimeSpan expired = Core::TimeSpan(_config.profileTimeAlive);
            if ( currProfile->isLoaded() && currProfile->inactiveTime() > expired )
            {
                SEISCOMP_INFO("Profile %s inactive for more than %f seconds",
                               currProfile->name.c_str(), expired.length());
                currProfile->unload();
            }
        }
    }
}



void RTDD::runNewJobs() 
{
    if ( --_cronCounter <= 0 )
    {
        // Reset counter
        _cronCounter = _config.wakeupInterval;

        Processes::iterator it;
        now = Core::Time::GMT();

        std::list<ProcessPtr> procToBeRemoved;
        // Update crontab
        for ( it = _processes.begin(); it != _processes.end(); it++)
        {
            ProcessPtr proc = it->second;
            CronjobPtr job = proc->cronjob;

            // Skip processes where nextRun is not set
            if ( job->runTimes.empty() )
            {
                SEISCOMP_DEBUG("Process %s expired, removing it",
                               proc->obj->publicID().c_str());
                procToBeRemoved.push_back(proc);
                continue;
            }

            Core::Time nextRun = job->runTimes.front();

            // Time of next run in future?
            if ( nextRun > now )
                continue;

            // Remove all times in the past
            while ( !job->runTimes.empty() && (job->runTimes.front() <= now) )
                job->runTimes.pop_front();

            // Add eventID to processQueue if not already inserted
            if ( find(_processQueue.begin(), _processQueue.end(), proc) ==
                 _processQueue.end() )
            {
                SEISCOMP_DEBUG("Pushing %s to process queue",
                               proc->obj->publicID().c_str());
                _processQueue.push_back(proc);
            }
        }

        for (ProcessPtr& proc : procToBeRemoved)
            removeProcess(proc.get());

        // Process event queue
        while ( !_processQueue.empty() )
        {
            ProcessPtr proc = _processQueue.front();
            _processQueue.pop_front();
            if ( startProcess(proc.get()) )
            {
                SEISCOMP_DEBUG("No more work to do for %s: removing job",
                               proc->obj->publicID().c_str());
                // nothing more to do, remove process
                removeProcess(proc.get());
            }
        }

        // Dump crontab if activated
        if ( _config.logCrontab )
        {
            ofstream of((Environment::Instance()->logDir() + "/" + name() + ".sched").c_str());
            of << "Now: " << now.toString("%F %T") << endl;
            of << "------------------------" << endl;
            of << "[Schedule]" << endl;
            for ( it = _processes.begin(); it != _processes.end(); it++)
            {
                ProcessPtr proc = it->second;
                CronjobPtr cronjob = proc->cronjob;
                if ( !cronjob->runTimes.empty() )
                    of << cronjob->runTimes.front().toString("%F %T") << "\t" << it->first
                       << "\t" << (cronjob->runTimes.front()-now).seconds() << endl;
                else
                    of << "STOPPED            \t" << it->first << endl;
            }

            // Dump process queue if not empty
            if ( !_processQueue.empty() ) {
                of << endl << "[Queue]" << endl;

                ProcessQueue::iterator it;
                for ( it = _processQueue.begin(); it != _processQueue.end(); ++it )
                    of << "WAITING            \t" << (*it)->obj->publicID() << endl;
            }
        }
    }
}


bool RTDD::addProcess(DataModel::PublicObject* obj)
{
    if (obj == nullptr) return false;

    _cache.feed(obj);

    now = Core::Time::GMT();

    // New process?
    ProcessPtr proc;
    Processes::iterator pit = _processes.find(obj->publicID());
    if ( pit == _processes.end() )
    {
        SEISCOMP_DEBUG("Adding process [%s]", obj->publicID().c_str());
        // create process
        proc = new Process;
        proc->created = now;
        proc->obj = obj;
        proc->cronjob = new Cronjob;
        // add process
        _processes[obj->publicID()] = proc;
    }
    else
    {
        SEISCOMP_DEBUG("Update process [%s]: resetting runTimes", obj->publicID().c_str());
        proc = pit->second;
    }

    // populate cronjob
    proc->cronjob->runTimes.clear();
    for ( size_t i = 0; i < _config.delayTimes.size(); ++i )
        proc->cronjob->runTimes.push_back(now + Core::TimeSpan(_config.delayTimes[i]));

    SEISCOMP_DEBUG("Update runTimes for [%s]",  proc->obj->publicID().c_str());

    handleTimeout();
    return true;
}



bool RTDD::startProcess(Process *proc)
{
    SEISCOMP_DEBUG("Starting process [%s]", proc->obj->publicID().c_str());

    OriginPtr org;

    // assume process contain an origin
    org = Origin::Cast(proc->obj);

    auto evalMode = Seiscomp::DataModel::AUTOMATIC;
    if ( org )
    {
        try{ evalMode = org->evaluationMode(); } catch ( ... ) {}
    }

    if ( org && evalMode == Seiscomp::DataModel::MANUAL )
    {
        // allow manual origins only if configured so
        if ( ! _config.processManualOrigin && !_config.forceProcessing )
        {
            SEISCOMP_DEBUG("Skip manual origin %s", org->publicID().c_str());
            return true;
        }
    }
    else if ( _config.onlyPreferredOrigin )
    {
        if ( ! org )
        {
            // fetch the preferred origin of the event
            EventPtr evt = Event::Cast(proc->obj);
            if ( evt )
            {
                org = _cache.get<Origin>(evt->preferredOriginID());
            }
        }
        else if ( !_config.forceProcessing )
        {
            SEISCOMP_DEBUG("Processing only preferred origin: skipping org %s",
                           org->publicID().c_str());
            return true;
        }
    }

    if ( ! org )
    {
        SEISCOMP_DEBUG("Nothing to do for process [%s]",  proc->obj->publicID().c_str());
        return true;
    }

    // return true when there is no more work to do
    return process(org.get());
}



void RTDD::removeProcess(Process *proc)
{
    // Remove process from process map
    Processes::iterator pit = _processes.find(proc->obj->publicID());
    if ( pit != _processes.end() ) _processes.erase(pit);

    // Remove process from queue
    ProcessQueue::iterator qit = find(_processQueue.begin(), _processQueue.end(), proc);
    if ( qit != _processQueue.end() ) _processQueue.erase(qit);
}



// return true when there is no more work to do
bool RTDD::process(Origin *origin)
{
    if ( !origin ) return true;

    SEISCOMP_DEBUG("Process origin %s",  origin->publicID().c_str());


    if ( startsWith(origin->methodID(), "RTDD", false) && !_config.forceProcessing )
    {
        SEISCOMP_DEBUG("Origin %s was generated by RTDD, skip it",
                      origin->publicID().c_str());
        return true;
    }

    if ( isAgencyIDBlocked(objectAgencyID(origin)) && !_config.forceProcessing )
    {
        SEISCOMP_DEBUG("%s: origin's agencyID '%s' is blocked",
                      origin->publicID().c_str(), objectAgencyID(origin).c_str());
        return true;
    }

    if ( !_config.forceProcessing )
    {
        // Check the origin hasn't been already processed and if it was processed
        // check the processing time is older than origin modification time
        if ( query() )
        {
            DatabaseIterator it;
            JournalEntryPtr entry;
            it = query()->getJournalAction(origin->publicID(), JOURNAL_ACTION);
            while ( (entry = static_cast<JournalEntry*>(*it)) != nullptr )
            {
                if ( entry->parameters() == JOURNAL_ACTION_COMPLETED &&
                     entry->created() >= origin->creationInfo().modificationTime() )
                {
                    SEISCOMP_DEBUG("%s: found journal entry \"completely processed\", ignoring origin",
                                  origin->publicID().c_str());
                    it.close();
                    return true;
                }
                ++it;
            }
            it.close();
            SEISCOMP_DEBUG("No journal entry \"completely processed\" found, go ahead");
        }
    }
    else
        SEISCOMP_DEBUG("Force processing, journal ignored");

    double latitude;
    double longitude;

    try {
        latitude  = origin->latitude().value();
        longitude = origin->longitude().value();
    }
    catch ( ... ) {
        SEISCOMP_WARNING("Ignoring origin %s with unset lat/lon",
                         origin->publicID().c_str());
        return true;
    }

    // Find best earth model based on region information and the initial origin
    ProfilePtr currProfile;

    for (list<ProfilePtr>::iterator it = _profiles.begin(); it != _profiles.end(); ++it )
    {
        if ( ! _config.forceProfile.empty() )
        {
            // if user forced a profile, use that
            if ( (*it)->name == _config.forceProfile)
                currProfile = *it;
        }
        else
        {
            // if epicenter is inside the configured region, use it
            if ( (*it)->region->isInside(latitude, longitude) )
                currProfile = *it;
        }
        if (currProfile) break;
    } 

    if ( !currProfile )
    {
        SEISCOMP_DEBUG("No profile found for location (lat:%s lon:%s), ignoring origin %s",
                       Core::toString(latitude).c_str(), Core::toString(longitude).c_str(),
                       origin->publicID().c_str());
        return true;
    }

    SEISCOMP_INFO("Relocating origin %s using profile %s",
                   origin->publicID().c_str(), currProfile->name.c_str());

    OriginPtr newOrg;

    try {
        newOrg = relocateOrigin(origin, currProfile);
    }
    catch ( exception &e ) {
        SEISCOMP_ERROR("%s", e.what());
        SEISCOMP_ERROR("Cannot relocate origin %s", origin->publicID().c_str());
        return false;
    }

    if ( !newOrg )
    {
        SEISCOMP_ERROR("processing of origin '%s' failed", origin->publicID().c_str());
        return false;
    }

    // finished processing, send new origin and update journal
    if (!send(newOrg.get()) )
        SEISCOMP_ERROR("%s: sending of derived origin failed", origin->publicID().c_str());

    SEISCOMP_INFO("Origin %s has been relocated", origin->publicID().c_str());

    if ( connection() && !_config.testMode )
    {
        DataModel::Journaling journal;
        JournalEntryPtr entry = new JournalEntry;
        entry->setObjectID(origin->publicID());
        entry->setAction(JOURNAL_ACTION);
        entry->setParameters(JOURNAL_ACTION_COMPLETED);
        entry->setSender(name() + "@" + Core::getHostname());
        entry->setCreated(Core::Time::GMT());
        Notifier::Enable();
        Notifier::Create(journal.publicID(), OP_ADD, entry.get());
        Notifier::Disable();

        Core::MessagePtr msg = Notifier::GetMessage();
        if ( msg ) connection()->send("EVENT", msg.get());
    }

    return true;
}



void RTDD::removedFromCache(Seiscomp::DataModel::PublicObject *po) {
    // do nothing
}



bool RTDD::send(Origin *org)
{
    if ( org == nullptr ) return false;

    logObject(_outputOrgs, Core::Time::GMT());

    if (!_config.eventXML.empty())
    {
        _eventParameters->add(org);
    }

    if ( _config.testMode ) return true;

    EventParametersPtr ep = new EventParameters;

    bool wasEnabled = Notifier::IsEnabled();
    Notifier::Enable();

    // Insert origin to event parameters
    ep->add(org);

    NotifierMessagePtr msg = Notifier::GetMessage();

    bool result = false;
    if ( connection() )
        result = connection()->send(msg.get());

    Notifier::SetEnabled(wasEnabled);

    return result;
}



OriginPtr RTDD::relocateOrigin(Origin *org, ProfilePtr profile)
{
    profile->load(query(), &_cache, _eventParameters.get(),
                  _config.workingDirectory, !_config.keepWorkingFiles,
                  _config.cacheWaveforms, false);

    HDD::CatalogPtr relocatedOrg = profile->relocateSingleEvent(org);
    // there must be only one event in the catalog, the relocated origin
    const HDD::Catalog::Event& event = relocatedOrg->getEvents().begin()->second;

    OriginPtr newOrg;

    if ( !_config.publicIDPattern.empty() )
    {
        newOrg = Origin::Create("");
        PublicObject::GenerateId(&*newOrg, _config.publicIDPattern);
    }
    else
        newOrg = Origin::Create();

    DataModel::CreationInfo ci;
    ci.setAgencyID(agencyID());
    ci.setAuthor(author());
    ci.setCreationTime(Core::Time::GMT());

    newOrg->setCreationInfo(ci);
    newOrg->setEarthModelID(profile->earthModelID);
    newOrg->setMethodID(profile->methodID);
    newOrg->setEvaluationMode(EvaluationMode(AUTOMATIC));
    newOrg->setEpicenterFixed(true);

    newOrg->setTime(DataModel::TimeQuantity(event.time));

    RealQuantity latitude = DataModel::RealQuantity(event.latitude);
    latitude.setUncertainty(event.relocInfo.latUncertainty);
    newOrg->setLatitude(latitude);

    RealQuantity longitude = DataModel::RealQuantity(event.longitude);
    longitude.setUncertainty(event.relocInfo.lonUncertainty);
    newOrg->setLongitude(longitude);

    RealQuantity depth = DataModel::RealQuantity(event.depth);
    depth.setUncertainty(event.relocInfo.depthUncertainty);
    newOrg->setDepth(depth);

    DataModel::Comment *comment = new DataModel::Comment();
    comment->setText(
        stringify("Cross-correlated P phases %d, S phases %d. Rms residual %.3f [sec]\n"
                  "Catalog P phases %d, S phases %d. Rms residual %.2f [sec]\n"
                  "Error [km]: East-west %.3f, north-south %.3f, depth %.3f",
                  event.relocInfo.numCCp, event.relocInfo.numCCs, event.relocInfo.rmsResidualCC,
                  event.relocInfo.numCTp, event.relocInfo.numCTs, event.relocInfo.rmsResidualCT,
                  event.relocInfo.lonUncertainty, event.relocInfo.latUncertainty,
                  event.relocInfo.depthUncertainty)
    );
    newOrg->add(comment);

    auto evPhases = relocatedOrg->getPhases().equal_range(event.id); // phases of relocated event 
    int usedPhaseCount = 0;
    double meanDist = 0;
    double minDist = std::numeric_limits<double>::max();
    double maxDist = 0;
    vector<double> azi;
    set<string> usedStations;

    // add arrivals
    for (size_t i = 0; i < org->arrivalCount(); i++)
    {
        DataModel::Arrival *orgArr = org->arrival(i);
        DataModel::PickPtr pick = _cache.get<DataModel::Pick>(orgArr->pickID());
        if ( !pick )
        {
            SEISCOMP_WARNING("Cannot find pick id %s. Cannot add Arrival to relocated origin",
                             orgArr->pickID().c_str());
            continue;
        }

        // prepare the new arrival
        DataModel::Arrival *newArr = new Arrival();
        newArr->setCreationInfo(ci);
        newArr->setPickID(org->arrival(i)->pickID());
        newArr->setPhase(org->arrival(i)->phase());
        try { newArr->setTimeCorrection(org->arrival(i)->timeCorrection()); }
        catch ( ... ) {}
        try { newArr->setWeight( org->arrival(i)->weight() ); }
        catch ( ... ) {}
        newArr->setTimeUsed(false);

        for (auto it = evPhases.first; it != evPhases.second; ++it)
        {
            const HDD::Catalog::Phase& phase = it->second;
            auto search = relocatedOrg->getStations().find(phase.stationId);
            if (search == relocatedOrg->getStations().end())
            {
                SEISCOMP_WARNING("Cannot find station id '%s' referenced by phase '%s'."
                                 "Cannot add Arrival to relocated origin",
                                 phase.stationId.c_str(), string(phase).c_str());
                continue;
            }
            const HDD::Catalog::Station& station = search->second;

            if (phase.time         == pick->time().value() &&
                phase.networkCode  ==  pick->waveformID().networkCode() &&
                phase.stationCode  ==  pick->waveformID().stationCode() &&
                phase.locationCode ==  pick->waveformID().locationCode() &&
                phase.channelCode  ==  pick->waveformID().channelCode() )
            {
                double distance, az, baz;
                Math::Geo::delazi(event.latitude, event.longitude,
                                  station.latitude, station.longitude,
                                  &distance, &az, &baz);
                newArr->setAzimuth(az);
                newArr->setDistance(distance);
                newArr->setTimeResidual( phase.relocInfo.isRelocated ? phase.relocInfo.residual : 0. );
                newArr->setWeight(phase.weight); //phase.relocInfo.finalWeight
                newArr->setTimeUsed(true);

                // update stats
                usedPhaseCount++;
                meanDist += distance;
                minDist = distance < minDist ? distance : minDist;
                maxDist = distance > maxDist ? distance : maxDist;
                azi.push_back(az);
                usedStations.insert(phase.stationId);
                break;
            }
        }
        newOrg->add(newArr);
    }
    // finish computing stats
    meanDist /= usedPhaseCount;

    double primaryAz = 360., secondaryAz = 360.;
    if (azi.size() >= 2)
    {
        primaryAz = secondaryAz = 0.;
        sort(azi.begin(), azi.end());
        vector<double>::size_type aziCount = azi.size();
        azi.push_back(azi[0] + 360.);
        azi.push_back(azi[1] + 360.);
        for (vector<double>::size_type i = 0; i < aziCount; i++)
        {
            double gap = azi[i+1] - azi[i];
            if (gap > primaryAz)
                primaryAz = gap;
            gap = azi[i+2] - azi[i];
            if (gap > secondaryAz)
                secondaryAz = gap;
        }
    }

    // add quality
    DataModel::OriginQuality oq;
    oq.setAssociatedPhaseCount(newOrg->arrivalCount());
    oq.setUsedPhaseCount(usedPhaseCount);
    try { oq.setAssociatedStationCount(org->quality().associatedStationCount()); }
    catch ( ... ) {}
    oq.setUsedStationCount(usedStations.size());
    oq.setStandardError(event.rms);
    oq.setMedianDistance(meanDist);
    oq.setMinimumDistance(minDist);
    oq.setMaximumDistance(maxDist);
    oq.setAzimuthalGap(primaryAz);
    oq.setSecondaryAzimuthalGap(secondaryAz);
    newOrg->setQuality(oq);

    // remember to add this entry to the catalog
    profile->addIncrementalCatalogEntry(newOrg.get());

    return newOrg;
}



// Profile class

RTDD::Profile::Profile()
{
    loaded = false;
}


void RTDD::Profile::load(DatabaseQuery* query,
                         PublicObjectTimeSpanBuffer* cache,
                         EventParameters* eventParameters,
                         const string& workingDir,
                         bool cleanupWorkingDir,
                         bool cacheWaveforms,
                         bool preloadData)
{
    if ( loaded ) return;

    string pWorkingDir = (boost::filesystem::path(workingDir)/name).string();

    SEISCOMP_INFO("Loading profile %s", name.c_str());

    this->query = query;
    this->cache = cache;
    this->eventParameters = eventParameters;

    // load the catalog either from seiscomp event/origin ids or from extended format
    HDD::CatalogPtr ddbgc;
    if ( ! eventIDFile.empty() )
    {
        HDD::DataSource dataSrc(query, cache, eventParameters);
        ddbgc = new HDD::Catalog();
        ddbgc->add(eventIDFile, dataSrc);
    }
    else
    {
        ddbgc = new HDD::Catalog(stationFile, eventFile, phaFile);
    }

    // if we have a incremental catalog, then load incremental entries
    if ( ! incrementalCatalogFile.empty() && Util::fileExists(incrementalCatalogFile) )
    { 
        HDD::DataSource dataSrc(query, cache, eventParameters);
        ddbgc->add(incrementalCatalogFile, dataSrc);
    }

    hypodd = new HDD::HypoDD(ddbgc, ddcfg, pWorkingDir);
    hypodd->setWorkingDirCleanup(cleanupWorkingDir);
    hypodd->setUseCatalogDiskCache(cacheWaveforms);
    hypodd->setUseSingleEvDiskCache(!incrementalCatalogFile.empty());
    loaded = true;
    lastUsage = Core::Time::GMT();

    if ( preloadData )
    {
        hypodd->preloadData();
    }
}


void RTDD::Profile::unload()
{
    SEISCOMP_INFO("Unloading profile %s", name.c_str());
    hypodd.reset();
    loaded = false;
    lastUsage = Core::Time::GMT();
}


HDD::CatalogPtr RTDD::Profile::relocateSingleEvent(Origin *org)
{
    if ( !loaded )
    {
        string msg = Core::stringify("Cannot relocate origin, profile %s not initialized", name);
        throw runtime_error(msg.c_str());
    }
    lastUsage = Core::Time::GMT();

    HDD::DataSource dataSrc(query, cache, eventParameters);

    // we pass the stations information from the background catalog, to avoid
    // wasting time accessing the inventory again for information we already have
    HDD::CatalogPtr orgToRelocate = new HDD::Catalog(
        hypodd->getCatalog()->getStations(),
        map<unsigned,HDD::Catalog::Event>(),
        multimap<unsigned,HDD::Catalog::Phase>()
    );
    orgToRelocate->add({org}, dataSrc);

    return hypodd->relocateSingleEvent(orgToRelocate);
}



HDD::CatalogPtr RTDD::Profile::relocateCatalog(bool force)
{
    if ( !loaded )
    {
        string msg = Core::stringify("Cannot relocate catalog, profile %s not initialized", name);
        throw runtime_error(msg.c_str());
    }
    lastUsage = Core::Time::GMT();
    return hypodd->relocateCatalog(force, ! ddcfg.ph2dt.ctrlFile.empty());
}



bool RTDD::Profile::addIncrementalCatalogEntry(Origin *org)
{
    if ( incrementalCatalogFile.empty() || ! org )
        return false;

    SEISCOMP_INFO("Adding origin %s to incremental catalog (profile %s file %s)",
                  org->publicID().c_str(), name.c_str(), incrementalCatalogFile.c_str());

    if ( ! Util::fileExists(incrementalCatalogFile) )
    {
        ofstream incStream(incrementalCatalogFile);
        incStream << "seiscompId" << endl;
        incStream << org->publicID() << endl;
    }
    else
    {
        ofstream incStream(incrementalCatalogFile, ofstream::app | ofstream::out);
        incStream << org->publicID() << endl;
    }
    // we could simply unload the profile and let the code upload it again with the
    // new event, but we don't want to lose all the cached waveforms that hypodd
    // has in memory
    HDD::DataSource dataSrc(query, cache, eventParameters);
    HDD::CatalogPtr newCatalog = new HDD::Catalog(*hypodd->getCatalog());
    newCatalog->add({org}, dataSrc);
    hypodd->setCatalog(newCatalog);

    return true;
}



// End Profile class

} // Seiscomp

