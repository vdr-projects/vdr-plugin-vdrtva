/*
 * vdrtva.c: A plugin for the Video Disk Recorder
 *
 * See the README file for copyright information and how to reach the author.
 *
 * $Id$
 */

#include <vdr/plugin.h>
#include <libsi/section.h>
#include <libsi/descriptor.h>
#include <stdarg.h>
#include <getopt.h>
#include <pwd.h>
#include "vdrtva.h"

#define REPORT(a...) void( (tvalog.mailFrom()) ? tvalog.Append(a) : tvasyslog(a) )


cChanDAs *ChanDAs;
cEventCRIDs *EventCRIDs;
cSuggestCRIDs *SuggestCRIDs;
cLinks *Links;

static const char *VERSION        = "0.1.1";
static const char *DESCRIPTION    = "TV-Anytime plugin";
//static const char *MAINMENUENTRY  = "vdrTva";

int collectionperiod;		// Time to collect all CRID data (default 10 minutes)
int lifetime;			// Lifetime of series link recordings (default 99)
int priority;			// Priority of series link recordings (default 99)
int seriesLifetime;		// Expiry time of a series link (default 30 days)
int updatetime;			// Time to carry out the series link update HHMM (default 03:00)
cTvaLog tvalog;


class cPluginvdrTva : public cPlugin {
private:
  // Add any member variables or functions you may need here.
  int state;
  time_t nextactiontime;
  char* configDir;
  cTvaFilter *Filter;
  cTvaStatusMonitor *statusMonitor;
  bool AppendItems(const char* Option);
  bool AddSeriesLink(const char *scrid, int modtime, const char *icrid);
  void LoadLinksFile(void);
  bool SaveLinksFile(void);
  bool UpdateLinksFromTimers(void);
  bool AddNewEventsToSeries(void);
  bool CheckSplitTimers(void);
  bool CreateTimerFromEvent(const cEvent *event);
  void CheckChangedEvents(void);
  void CheckTimerClashes(void);
  void FindAlternatives(const cEvent *event);
  void StartDataCapture(void);
  void StopDataCapture(void);
  void Update(void);
  void Check(void);
  void tvasyslog(const char *Fmt, ...);

public:
  cPluginvdrTva(void);
  virtual ~cPluginvdrTva();
  virtual const char *Version(void) { return VERSION; }
  virtual const char *Description(void) { return tr(DESCRIPTION); }
  virtual const char *CommandLineHelp(void);
  virtual bool ProcessArgs(int argc, char *argv[]);
  virtual bool Initialize(void);
  virtual bool Start(void);
  virtual void Stop(void);
  virtual void Housekeeping(void);
  virtual void MainThreadHook(void);
  virtual cString Active(void);
  virtual time_t WakeupTime(void);
  virtual cMenuSetupPage *SetupMenu(void);
  virtual bool SetupParse(const char *Name, const char *Value);
  virtual bool Service(const char *Id, void *Data = NULL);
  virtual const char **SVDRPHelpPages(void);
  virtual cString SVDRPCommand(const char *Command, const char *Option, int &ReplyCode);
  };

cPluginvdrTva::cPluginvdrTva(void)
{
  // Initialize any member variables here.
  // DON'T DO ANYTHING ELSE THAT MAY HAVE SIDE EFFECTS, REQUIRE GLOBAL
  // VDR OBJECTS TO EXIST OR PRODUCE ANY OUTPUT!
  configDir = NULL;
  Filter = NULL;
  ChanDAs = NULL;
  EventCRIDs = NULL;
  SuggestCRIDs = NULL;
  Links = NULL;
  seriesLifetime = 30 * SECSINDAY;
  priority = 99;
  lifetime = 99;
  state = 0;
  collectionperiod = 10 * 60;
  updatetime = 300;
}

cPluginvdrTva::~cPluginvdrTva()
{
  // Clean up after yourself!
}

const char *cPluginvdrTva::CommandLineHelp(void)
{
  // Return a string that describes all known command line options.
  return "  -l n     --lifetime=n       Lifetime of new timers (default 99)\n"
	 "  -m addr  --mailaddr=addr    Address to send mail report\n"
	 "  -p n     --priority=n       Priority of new timers (default 99)\n"
	 "  -s n     --serieslifetime=n Days to remember a series after the last event (default 30)\n"
	 "  -u HH:MM --updatetime=HH:MM Time to update series links (default 03:00)\n";
}

bool cPluginvdrTva::ProcessArgs(int argc, char *argv[])
{
  // Implement command line argument processing here if applicable.
  static struct option long_options[] = {
       { "serieslifetime", required_argument, NULL, 's' },
       { "priority",       required_argument, NULL, 'p' },
       { "lifetime",       required_argument, NULL, 'l' },
       { "updatetime",     required_argument, NULL, 'u' },
       { "mailaddr",	   required_argument, NULL, 'm' },
       { NULL }
     };

  int c, opt;
  char *hours, *mins, *strtok_next;
  char buf[32];
  while ((c = getopt_long(argc, argv, "l:m:p:s:u:", long_options, NULL)) != -1) {
    switch (c) {
      case 'l':
	opt = atoi(optarg);
	if (opt > 0) lifetime = opt;
	break;
      case 'm':
	tvalog.setmailTo(optarg);
	break;
      case 'p':
	opt = atoi(optarg);
	if (opt > 0) priority = opt;
	break;
      case 's':
	opt = atoi(optarg);
	if (opt > 0) seriesLifetime = opt * SECSINDAY;
	break;
      case 'u':
	strncpy(buf, optarg,sizeof(buf));
	hours = strtok_r(buf, ":", &strtok_next);
	mins = strtok_r(NULL, "!", &strtok_next);
	updatetime = atoi(hours)*100 + atoi(mins);
	break;
      default:
	return false;
    }
  }
  return true;
}

bool cPluginvdrTva::Initialize(void)
{
  // Initialize any background activities the plugin shall perform.
  return true;
}

bool cPluginvdrTva::Start(void)
{
  // Start any background activities the plugin shall perform.
  configDir = strcpyrealloc(configDir, cPlugin::ConfigDirectory("vdrtva"));
  LoadLinksFile();
  statusMonitor = new cTvaStatusMonitor;
  if (tvalog.mailTo()) {
    struct stat sb;
    if (stat("/usr/sbin/sendmail", &sb) == 0) {
      char hostname[256];
      if (!gethostname (hostname, sizeof(hostname))) {
	char buf[16384];
	struct passwd pwd, *result;
	size_t bufsize = sizeof(buf);
	int s = getpwuid_r (getuid(), &pwd, buf, bufsize, &result);
	if ((s == 0) && (result != NULL)) {
	  char from[256];
	  sprintf(from, "%s@%s", pwd.pw_name, hostname);
	  tvalog.setmailFrom(from);
	  isyslog("vdrtva: daily reports will be mailed from %s to %s", from, tvalog.mailTo());
	}
	else {
	  esyslog("vdrtva: unable to establish vdr's user name");
	}
      }
      else {
	esyslog("vdrtva: unable to establish vdr's hostname");
      }
    }
    else {
      esyslog("vdrtva: no mail server found");
    }
  }
  struct tm tm_r;
  char buff[32];
  time_t now = time(NULL);
  localtime_r(&now, &tm_r);
  tm_r.tm_sec = 0;
  tm_r.tm_hour = updatetime / 100;
  tm_r.tm_min = updatetime % 100;
  nextactiontime = mktime(&tm_r);
  if (nextactiontime < now) nextactiontime += SECSINDAY;
  ctime_r(&nextactiontime, buff);
  REPORT("Vdrtva initialised, next update due at %s", buff);
  return true;
}

void cPluginvdrTva::Stop(void)
{
  // Stop any background activities the plugin is performing.
  if (Filter) {
    delete Filter;
    Filter = NULL;
  }
  tvalog.MailLog();
  if(statusMonitor) delete statusMonitor;
}

void cPluginvdrTva::Housekeeping(void)
{
  // Perform any cleanup or other regular tasks.
  if (nextactiontime < time(NULL)) {
    statusMonitor->ClearTimerAdded();		// Ignore any timer changes while update is in progress
    switch (state) {
      case 0:
	StartDataCapture();
	nextactiontime += collectionperiod;
	state++;
	break;
      case 1:
	StopDataCapture();
	state++;
	break;
      case 2:
	Update();
	state++;
	break;
      case 3:
	Check();
	nextactiontime += (SECSINDAY - collectionperiod);
	state = 0;
	tvalog.MailLog();
	break;
    }
  }
  else if (EventCRIDs && statusMonitor->GetTimerAddedDelta() > 60) {
    Update();			// Wait 1 minute for VDR to enter the event data into the new timer.
    Check();
    statusMonitor->ClearTimerAdded();
  }
}

void cPluginvdrTva::MainThreadHook(void)
{
  // Perform actions in the context of the main program thread.
  // WARNING: Use with great care - see PLUGINS.html!
}

cString cPluginvdrTva::Active(void)
{
  // Return a message string if shutdown should be postponed
  return NULL;
}

time_t cPluginvdrTva::WakeupTime(void)
{
  // Return custom wakeup time for shutdown script
  return 0;
}

cMenuSetupPage *cPluginvdrTva::SetupMenu(void)
{
  // Return a setup menu in case the plugin supports one.
  return new cTvaMenuSetup;
}

bool cPluginvdrTva::SetupParse(const char *Name, const char *Value)
{
  // Parse your own setup parameters and store their values.
  if      (!strcasecmp(Name, "CollectionPeriod")) collectionperiod = atoi(Value) * 60;
  else if (!strcasecmp(Name, "SeriesLifetime")) seriesLifetime = atoi(Value) * SECSINDAY;
  else if (!strcasecmp(Name, "TimerLifetime")) lifetime = atoi(Value);
  else if (!strcasecmp(Name, "TimerPriority")) priority = atoi(Value);
  else if (!strcasecmp(Name, "UpdateTime")) updatetime = atoi(Value);
  else return false;
  return true;
}

bool cPluginvdrTva::Service(const char *Id, void *Data)
{
  // Handle custom service requests from other plugins
  return false;
}

const char **cPluginvdrTva::SVDRPHelpPages(void)
{
  // Return help text for SVDRP commands this plugin implements
   static const char *HelpPages[] = {
    "LLOG\n"
    "    Print the action log.",
    "LSTL\n"
    "    Print the Links list.",
    "LSTS\n"
    "    Print the suggested events list",
    "LSTY\n"
    "    Print the Event list including CRIDs.",
    "LSTZ\n"
    "    Print the channel list with Default Authority.",
    "STOP\n"
    "    Stop Event data capture (retaining data).",
    "STRT\n"
    "    Start Event data capture (erasing any existing data)",
    "UPDT\n"
    "    Update timers and links (series link functionality)",
    NULL
    };
  return HelpPages;
}

cString cPluginvdrTva::SVDRPCommand(const char *Command, const char *Option, int &ReplyCode)
{
  // Process SVDRP commands this plugin implements
  cTvaLog reply;
  isyslog ("vdrtva: processing command %s", Command);
  if (strcasecmp(Command, "LLOG") == 0) {
    ReplyCode = 250;
    if (tvalog.Length() > 0) return cString(tvalog.Buffer());
    else return cString::sprintf("Nothing in the buffer!");
  }
  else if (strcasecmp(Command, "LSTL") == 0) {
    if (Links && (Links->MaxNumber() >=1)) {
      ReplyCode = 250;
      for (cLinkItem *linkItem = Links->First(); linkItem; linkItem = Links->Next(linkItem)) {
	reply.Append("%s,%d;%s\n", linkItem->sCRID(), linkItem->ModTime(), linkItem->iCRIDs());
      }
    }
    if (reply.Length() > 0) return cString(reply.Buffer());
    else return cString::sprintf("Nothing in the buffer!");
  }
  else if (strcasecmp(Command, "LSTS") == 0) {
    if (SuggestCRIDs && (SuggestCRIDs->MaxNumber() >= 1)) {
      ReplyCode = 250;
      cSuggestCRID *suggest = SuggestCRIDs->First();
      while (suggest) {
	cSuggestCRID *next = SuggestCRIDs->Next(suggest);
	cChanDA *chanDA = ChanDAs->GetByChannelID(suggest->Cid());
	if(chanDA) {
	  reply.Append("%s%s %s%s\n", chanDA->DA(), suggest->iCRID(), chanDA->DA(), suggest->gCRID());
	}
	suggest = next;
      }
      if (reply.Length() > 0) return cString(reply.Buffer());
      else return cString::sprintf("Nothing in the buffer!");
    }
    else
      return cString::sprintf("No suggested events defined");
  }
  else if (strcasecmp(Command, "LSTY") == 0) {
    if (EventCRIDs && (EventCRIDs->MaxNumber() >= 1)) {
       ReplyCode = 250;
       for (cEventCRID *eventCRID = EventCRIDs->First(); eventCRID; eventCRID = EventCRIDs->Next(eventCRID)) {
	  cChanDA *chanDA = ChanDAs->GetByChannelID(eventCRID->Cid());
	  if(chanDA) {
            reply.Append("%d %d %s%s %s%s\n", chanDA->Cid(), eventCRID->Eid(), chanDA->DA(), eventCRID->iCRID(), chanDA->DA(), eventCRID->sCRID());
	  }
	}
	if (reply.Length() > 0) return cString(reply.Buffer());
	else return cString::sprintf("Nothing in the buffer!");
    }
    else
       return cString::sprintf("No events defined");
  }
  else if (strcasecmp(Command, "LSTZ") == 0) {
    if (ChanDAs && (ChanDAs->MaxNumber() >= 1)) {
       ReplyCode = 250;
       for (cChanDA *chanDA = ChanDAs->First(); chanDA; chanDA = ChanDAs->Next(chanDA)) {
          reply.Append("%d %s\n", chanDA->Cid(), chanDA->DA());
       }
	if (reply.Length() > 0) return cString(reply.Buffer());
	else return cString::sprintf("Nothing in the buffer!");
    }
    else
       return cString::sprintf("No channels defined");
  }
  else if (strcasecmp(Command, "STRT") == 0) {
    if (!Filter) {
      StartDataCapture();
      return cString::sprintf("Data capture started");
    }
    else {
       ReplyCode = 999;
      return cString::sprintf("Double start attempted");
    }
  }
  else if (strcasecmp(Command, "STOP") == 0) {
    if (Filter) {
      StopDataCapture();
      return cString::sprintf("Data capture stopped");
    }
    else {
      ReplyCode = 999;
      return cString::sprintf("Double stop attempted");
    }
  }
  else if (strcasecmp(Command, "UPDT") == 0) {
    if (EventCRIDs) {
      Update();
      Check();
      return cString::sprintf("Update completed");
    }
    else {
      ReplyCode = 999;
      return cString::sprintf("Update attempted before data capture");
    }
  }
  return NULL;
}

void cPluginvdrTva::StartDataCapture()
{
  if (!Filter) {
    if (EventCRIDs) delete EventCRIDs;
    if (ChanDAs) delete ChanDAs;
    if (SuggestCRIDs) delete SuggestCRIDs;
    EventCRIDs = new cEventCRIDs();
    SuggestCRIDs = new cSuggestCRIDs;
    ChanDAs = new cChanDAs();
    Filter = new cTvaFilter();
    cDevice::ActualDevice()->AttachFilter(Filter);
    isyslog("vdrtva: Data capture started");
  }
}

void cPluginvdrTva::StopDataCapture()
{
  if (Filter) {
    delete Filter;
    Filter = NULL;
    if (SuggestCRIDs && (SuggestCRIDs->MaxNumber() >= 1)) {	// De-dup the suggestions list.
      SuggestCRIDs->Sort();
      cSuggestCRID *suggest = SuggestCRIDs->First();
      while (suggest) {
	cSuggestCRID *next = SuggestCRIDs->Next(suggest);
	if (next && !strcmp(next->iCRID(), suggest->iCRID()) && !strcmp(next->gCRID(), suggest->gCRID())) {
	  SuggestCRIDs->Del(suggest);
	}
	suggest = next;
      }
    }
    isyslog("vdrtva: Data capture stopped");
  }
}

void cPluginvdrTva::Update()
{
  bool status = UpdateLinksFromTimers();
  status |= AddNewEventsToSeries();
  if(status) SaveLinksFile();
  isyslog("vdrtva: Updates complete");
}

void cPluginvdrTva::Check()
{
  CheckChangedEvents();
  CheckTimerClashes();
  CheckSplitTimers();
  isyslog("vdrtva: Checks complete");
}

// add a new event to the Links table, either as an addition to an existing series or as a new series.
// return false = nothing done, true = new event for old series, or new series.

bool cPluginvdrTva::AddSeriesLink(const char *scrid, int modtime, const char *icrid)
{
  if (Links && (Links->MaxNumber() >=1)) {
    for (cLinkItem *Item = Links->First(); Item; Item = Links->Next(Item)) {
      if (strcasecmp(Item->sCRID(), scrid) == 0) {
	if (strstr(Item->iCRIDs(), icrid) == NULL) {
	  cString icrids = cString::sprintf("%s:%s", Item->iCRIDs(), icrid);
	  modtime = max(Item->ModTime(), modtime);
	  Item->Set(Item->sCRID(), modtime, icrids);
	  REPORT("Adding new event %s to series %s\n", icrid, scrid);
	  return true;
	}
	return false;
      }
    }
  }
  Links->NewLinkItem(scrid, modtime, icrid);
  REPORT("Creating new series %s for event %s\n", scrid, icrid);
  return true;
}

void cPluginvdrTva::LoadLinksFile()
{
  Links = new cLinks();
  cString curlinks = AddDirectory(configDir, "links.data");
  FILE *f = fopen(curlinks, "r");
  if (f) {
    char *s;
    char *strtok_next;
    cReadLine ReadLine;
    cLinkItem *LinkItem;
    int modtime;
    while ((s = ReadLine.Read(f)) != NULL) {
      char *scrid = strtok_r(s, ",", &strtok_next);
      char *mtime = strtok_r(NULL, ";", &strtok_next);
      char *icrids = strtok_r(NULL, "!", &strtok_next);
      modtime = atoi(mtime);
      LinkItem = Links->NewLinkItem(scrid, modtime, icrids);
    }
    fclose (f);
    isyslog("vdrtva: loaded %d series links\n", Links->MaxNumber());
  }
  else esyslog("vdrtva: series links file not found\n");
}
  
bool cPluginvdrTva::SaveLinksFile()
{
  cString curlinks = AddDirectory(configDir, "links.data");
  cString newlinks = AddDirectory(configDir, "links.new");
  cString oldlinks = AddDirectory(configDir, "links.old");
  FILE *f = fopen(newlinks, "w");
  if (f) {
    cLinkItem *Item = Links->First();
    while (Item) {
      cLinkItem *next = Links->Next(Item);
      if ((Item->ModTime() + seriesLifetime) > time(NULL)) {
	fprintf(f, "%s,%d;%s\n", Item->sCRID(), Item->ModTime(), Item->iCRIDs());
      }
      else {
	isyslog ("vdrtva: Expiring series %s\n", Item->sCRID());
	Links->Del(Item);
      }
      Item = next;
    }
    fclose(f);
    unlink (oldlinks);		// Allow to fail if the save file does not exist
    rename (curlinks, oldlinks);
    rename (newlinks, curlinks);
  }
  return true;
}

// Check that all timers are part of series links and update the links.

bool cPluginvdrTva::UpdateLinksFromTimers()
{
  if ((Timers.Count() == 0) || (!EventCRIDs)) return false;
  bool status = false;
  for (int i = 0; i < Timers.Count(); i++) {
    cTimer *timer = Timers.Get(i);
    if (timer) {
// find the event for this timer
      const cEvent *event = timer->Event();
      if (event) {
	cChannel *channel = Channels.GetByChannelID(event->ChannelID());
// find the sCRID and iCRID for the event
	cChanDA *chanda = ChanDAs->GetByChannelID(channel->Number());
	cEventCRID *eventcrid = EventCRIDs->GetByID(channel->Number(), event->EventID());
	if (eventcrid && chanda) {
	  cString scrid = cString::sprintf("%s%s", chanda->DA(),eventcrid->sCRID());
	  cString icrid = cString::sprintf("%s%s", chanda->DA(),eventcrid->iCRID());
// scan the links table for the sCRID
//   if found, check if the iCRID is present, if not add it
//   else create a new links entry
	  status |= AddSeriesLink(scrid, event->StartTime(), icrid);
	}
      }
    }
  }
  return status;
}

// Find new events for series links and create timers for them.

bool cPluginvdrTva::AddNewEventsToSeries()
{
  bool saveNeeded = false;
  if (!Links || (Links->MaxNumber() < 1)) return false;
// Foreach CRID
  for (cEventCRID *eventCRID = EventCRIDs->First(); eventCRID; eventCRID = EventCRIDs->Next(eventCRID)) {
    cChanDA *chanDA = ChanDAs->GetByChannelID(eventCRID->Cid());
    if (chanDA) {
// Check for an entry in the Links table with the same sCRID
      cString scrid = cString::sprintf("%s%s", chanDA->DA(),eventCRID->sCRID());
      for (cLinkItem *Item = Links->First(); Item; Item = Links->Next(Item)) {
	if (strcasecmp(Item->sCRID(), scrid) == 0) {
// if found, look for the event's icrid in ALL series
	  cString icrid = cString::sprintf("%s%s", chanDA->DA(),eventCRID->iCRID());
	  bool done = false;
	  for (cLinkItem *Item2 = Links->First(); Item2; Item2 = Links->Next(Item2)) {
	    if (strstr(Item2->iCRIDs(), icrid) != NULL) {
	      done = true;
	    }
	  }
// if not found, add a new timer for the event and update the series.
	  if (!done) {
	    cChannel *channel = Channels.GetByNumber(eventCRID->Cid());
	    cSchedulesLock SchedulesLock;
	    const cSchedules *Schedules = cSchedules::Schedules(SchedulesLock);
	    if (Schedules) {
	      const cSchedule *schedule = Schedules->GetSchedule(channel);
	      if (schedule) {
		const cEvent *event = schedule->GetEvent(eventCRID->Eid());
		if (CreateTimerFromEvent(event)) {
		  AddSeriesLink(scrid, event->StartTime(), icrid);
		  saveNeeded = true;
		}
	      }
	    }
	  }
	}
      }
    }
  }
  return saveNeeded;
}

// Check timers to see if the event they were set to record is still in the EPG.
// This won't work if the start time is padded.

void cPluginvdrTva::CheckChangedEvents()
{
  if (Timers.Count() == 0) return;
  for (int i = 0; i < Timers.Count(); i++) {
    cTimer *timer = Timers.Get(i);
    if (timer) {
      const cChannel *channel = timer->Channel();
      cSchedulesLock SchedulesLock;
      const cSchedules *Schedules = cSchedules::Schedules(SchedulesLock);
      if (Schedules) {
	const cSchedule *schedule = Schedules->GetSchedule(channel);
	if (schedule) {
	  const cEvent *event = schedule->GetEvent(NULL, timer->StartTime());
	  if (!event) REPORT("Event for timer '%s' at %s seems to no longer exist", timer->File(), *DayDateTime(timer->StartTime()));
	  else if (strcmp(timer->File(), event->Title())) {
	    REPORT("Changed timer event at %s: %s <=> %s", *DayDateTime(timer->StartTime()), timer->File(), event->Title());
	  }
	}
      }
    }
  }
}

// Check for timer clashes - overlapping timers which are not on the same transponder.
// FIXME How to deal with multiple input devices??

void cPluginvdrTva::CheckTimerClashes(void)
{
  if (Timers.Count() < 2) return;
  for (int i = 1; i < Timers.Count(); i++) {
    cTimer *timer1 = Timers.Get(i);
    if (timer1) {
      for (int j = 0; j < i; j++) {
	cTimer *timer2 = Timers.Get(j);
	if (timer2) {
	  if((timer1->StartTime() >= timer2->StartTime() && timer1->StartTime() < timer2->StopTime())
	   ||(timer2->StartTime() >= timer1->StartTime() && timer2->StartTime() < timer1->StopTime())) {
	    const cChannel *channel1 = timer1->Channel();
	    const cChannel *channel2 = timer2->Channel();
	    if (channel1->Transponder() != channel2->Transponder()) {
	      REPORT("Timer collision at %s. %s <=> %s", *DayDateTime(timer1->StartTime()), timer1->File(), timer2->File());
	      FindAlternatives(timer1->Event());
	      FindAlternatives(timer2->Event());
	    }
	  }
	}
      }
    }
  }
}

// Find alternative broadcasts for an event, ie events in the EIT having the same CRID.
// Check whether adding a timer for the alternative would create a clash with any other timer.

void cPluginvdrTva::FindAlternatives(const cEvent *event)
{
  if (!event) {
    dsyslog("vdrtva: FindAlternatives() called without Event!");
    return;
  }
  cChannel *channel = Channels.GetByChannelID(event->ChannelID());
  cChanDA *chanda = ChanDAs->GetByChannelID(channel->Number());
  cEventCRID *eventcrid = EventCRIDs->GetByID(channel->Number(), event->EventID());
  if (!eventcrid || !chanda) {
    REPORT("Cannot find alternatives for '%s' - no series link data", event->Title());
    return;
  }
  bool found = false;
  for (cEventCRID *eventcrid2 = EventCRIDs->First(); eventcrid2; eventcrid2 = EventCRIDs->Next(eventcrid2)) {
    if ((strcmp(eventcrid->iCRID(), eventcrid2->iCRID()) == 0) && (event->EventID() != eventcrid2->Eid())) {
      cChanDA *chanda2 = ChanDAs->GetByChannelID(eventcrid2->Cid());
      if (strcmp(chanda->DA(), chanda2->DA()) == 0) {
	cChannel *channel2 = Channels.GetByNumber(eventcrid2->Cid());
	cSchedulesLock SchedulesLock;
	const cSchedules *schedules = cSchedules::Schedules(SchedulesLock);
	if (schedules) {
	  const cSchedule *schedule = schedules->GetSchedule(channel2);
	  if (schedule) {
	    const cEvent *event2 = schedule->GetEvent(eventcrid2->Eid(), 0);
	    if (!found) {
	      REPORT("Alternatives for '%s':", event->Title());
	      found = true;
	    }
	    bool clash = false;
	    for (int i = 1; i < Timers.Count(); i++) {
	      cTimer *timer = Timers.Get(i);
	      if (timer) {
		if((timer->StartTime() >= event2->StartTime() && timer->StartTime() < event2->EndTime())
		  ||(event2->StartTime() >= timer->StartTime() && event2->StartTime() < timer->StopTime())) {
		  cChannel *channel = Channels.GetByChannelID(event2->ChannelID());
		  if (timer->Channel()->Transponder() != channel->Transponder()) {
		    REPORT("%s %s (clash with timer '%s')", channel2->Name(), *DayDateTime(event2->StartTime()), timer->File());
		    clash = true;
		  }
		}
	      }
	    }
	    if (!clash) {
	      REPORT("%s %s", channel2->Name(), *DayDateTime(event2->StartTime()));
	    }
	  }
	}
      }
    }
  }
  if (!found) REPORT("No alternatives for '%s'", event->Title());
}

// Check that, if any split events (eg a long programme with a news break in the middle)
// are being recorded, that timers are set for all of the parts.
// FIXME This may not work if the programme is being repeated. Inefficient algorithm.

bool cPluginvdrTva::CheckSplitTimers(void)
{
  if (Timers.Count() == 0) return false;
  for (int i = 0; i < Timers.Count(); i++) {
    cTimer *timer = Timers.Get(i);
    if (timer) {
      const cEvent *event = timer->Event();
      if (event) {
	cChannel *channel = Channels.GetByChannelID(event->ChannelID());
	cChanDA *chanda = ChanDAs->GetByChannelID(channel->Number());
	cEventCRID *eventcrid = EventCRIDs->GetByID(channel->Number(), event->EventID());
	if (eventcrid && chanda && strchr(eventcrid->iCRID(), '#')) {
//	  char crid[Utf8BufSize(256)], *next;
//	  strcpy(crid, eventcrid->iCRID());
//	  char *prefix = strtok_r(crid, "#", &next);
//	  char *suffix = strtok_r(NULL, "#", &next);
	  REPORT("Timer for split event '%s' found - check all parts are being recorded!", event->Title());
	}
      }
    }
  }
  return false;
}

// Create a timer from an event, setting VPS parameter explicitly.

bool cPluginvdrTva::CreateTimerFromEvent(const cEvent *event) {
  struct tm tm_r;
  char startbuff[64], endbuff[64], etitle[256];
  int flags;
  cChannel *channel = Channels.GetByChannelID(event->ChannelID());
  time_t starttime = event->StartTime();
  time_t endtime = event->EndTime();
  if (!Setup.UseVps) {
    starttime -= Setup.MarginStart * 60;
    endtime += Setup.MarginStop * 60;
    flags = 1;
  }
  else flags = 5;
  localtime_r(&starttime, &tm_r);
  strftime(startbuff, sizeof(startbuff), "%Y-%m-%d:%H%M", &tm_r);
  localtime_r(&endtime, &tm_r);
  strftime(endbuff, sizeof(endbuff), "%H%M", &tm_r);
  strn0cpy (etitle, event->Title(), sizeof(etitle));
  strreplace(etitle, ':', '|');
  cString timercmd = cString::sprintf("%u:%d:%s:%s:%d:%d:%s:\n", flags, channel->Number(), startbuff, endbuff, priority, lifetime, etitle);
  cTimer *timer = new cTimer;
  if (timer->Parse(timercmd)) {
    cTimer *t = Timers.GetTimer(timer);
    if (!t) {
      timer->SetEvent(event);
      Timers.Add(timer);
      Timers.SetModified();
      REPORT("Timer created for '%s' on %s, %s %04d-%04d", etitle, channel->Name(), *DateString(starttime), timer->Start(), timer->Stop());
      return true;
    }
    isyslog("vdrtva: Duplicate timer creation attempted for %s on %s", *timer->ToDescr(), *DateString(timer->StartTime()));
  }
  return false;
}

//	Report actions to syslog if we don't want an email.

void cPluginvdrTva::tvasyslog(const char *Fmt, ...) {
  
  va_list ap;
  char buff[4096];

  va_start(ap, Fmt);
  vsnprintf(buff, sizeof(buff), Fmt, ap);
  va_end(ap);
  isyslog("vdrtva: %s", buff);
}



/*
	cTvaStatusMonitor - callback for timer changes.
*/

cTvaStatusMonitor::cTvaStatusMonitor(void)
{
  timeradded = NULL;
  lasttimer = NULL;
}

void cTvaStatusMonitor::TimerChange(const cTimer *Timer, eTimerChange Change)
{
  if (Change == tcAdd) {
    timeradded = time(NULL);
    lasttimer = Timer;
  }
}

int cTvaStatusMonitor::GetTimerAddedDelta(void)
{
  if (timeradded) {
    return (time(NULL) - timeradded);
  }
  return 0;
}

void cTvaStatusMonitor::ClearTimerAdded(void)
{
  timeradded = NULL;
  return;
}


/*
	cTvaMenuSetup - setup menu function.
*/

cTvaMenuSetup::cTvaMenuSetup(void)
{
  newcollectionperiod = collectionperiod / 60;
  newlifetime = lifetime;
  newpriority = priority;
  newseriesLifetime = seriesLifetime / SECSINDAY;
  newupdatetime = updatetime;
  Add(new cMenuEditIntItem(tr("Collection period (min)"), &newcollectionperiod, 1, 99));
  Add(new cMenuEditIntItem(tr("Series link lifetime (days)"), &newseriesLifetime, 1, 366));
  Add(new cMenuEditIntItem(tr("New timer lifetime"), &newlifetime, 0, 99));
  Add(new cMenuEditIntItem(tr("New timer priority"), &newpriority, 0, 99));
  Add(new cMenuEditTimeItem(tr("Update Time (HH:MM)"), &newupdatetime));
}

void cTvaMenuSetup::Store(void)
{
  SetupStore("CollectionPeriod", newcollectionperiod); collectionperiod = newcollectionperiod * 60;
  SetupStore("SeriesLifetime", newseriesLifetime); seriesLifetime = newseriesLifetime * SECSINDAY;
  SetupStore("TimerLifetime", newlifetime); lifetime = newlifetime;
  SetupStore("TimerPriority", newpriority); priority = newpriority;
  SetupStore("UpdateTime", newupdatetime); updatetime = newupdatetime;
}


/*
	cTvaLog - logging class
*/

cTvaLog::cTvaLog(void) {
  buffer = mailfrom = mailto = NULL;
}

cTvaLog::~cTvaLog(void) {
  if (buffer) free(buffer);
  if (mailfrom) free(mailfrom);
  if (mailto) free(mailto);
}

//	Append an entry to the log. Ensure the entry is CR-terminated.

void cTvaLog::Append(const char *Fmt, ...)
{
  va_list ap;

  if (!buffer) {
    length = 0;
    size = 4096;
    buffer = (char *) malloc(sizeof(char) * size);
  }
  while (buffer) {
    va_start(ap, Fmt);
    int n = vsnprintf(buffer + length, size - length, Fmt, ap);
    va_end(ap);
    if (n < size - length - 1) {
      length += n;
      if (*(buffer+length-1) != '\n') {
	*(buffer+length) = '\n';
	length++;
	*(buffer+length) = '\0';
      }
      return;
    }
// overflow: realloc and try again
    size *= 2;
    char *tmp = (char *) realloc(buffer, sizeof(char) * size);
    if (!tmp) free(buffer);
    buffer = tmp;
  }
  return;
}

int cTvaLog::Length(void) {
  if (!buffer) return 0;
  return length;
}

void cTvaLog::setmailTo(char *opt) {
  mailto = strcpyrealloc(mailto, opt);
}

void cTvaLog::setmailFrom(char *opt) {
  mailfrom = strcpyrealloc(mailfrom, opt);
}

//	Mail out the daily report.

void cTvaLog::MailLog(void) {
FILE* mail;
char mailcmd[256];

  if (length == 0) return;

  snprintf(mailcmd, sizeof(mailcmd), "/usr/sbin/sendmail -i -oem  %s", mailto);
  if (!(mail = popen(mailcmd, "w"))) {
    esyslog("vdrtva: cannot open sendmail");
    return;
  }
  fprintf(mail, "From: %s\n", mailfrom);
  fprintf(mail, "To: %s\n", mailto);
  fprintf(mail, "Subject: vdrTva report\n");
//  fprintf(mail, "Content-Type: text/plain; charset=%s\n", GetCodeset().c_str());
  fprintf(mail, "\n");
  fputs(buffer, mail);
  pclose(mail);
  Clear();
}


/*
	cTvaFilter - capture the CRID data from EIT.
*/

cTvaFilter::cTvaFilter(void)
{
  Set(0x11, 0x42);        // SDT (Actual)
  Set(0x11, 0x46);        // SDT (Other)
  Set(0x12, 0x40, 0xC0);  // event info, actual(0x4E)/other(0x4F) TS, present/following
			  // event info, actual TS, schedule(0x50)/schedule for future days(0x5X)
			  // event info, other  TS, schedule(0x60)/schedule for future days(0x6X)
}

void cTvaFilter::Process(u_short Pid, u_char Tid, const u_char *Data, int Length)
{
  // do something with the data here
  switch (Pid) {
    case 0x11: {
      sectionSyncer.Reset();
      SI::SDT sdt(Data, false);
      if (!sdt.CheckCRCAndParse()) {
        dsyslog ("vdrtva: SDT Parse / CRC error\n");
        return;
      }
      if (!sectionSyncer.Sync(sdt.getVersionNumber(), sdt.getSectionNumber(), sdt.getLastSectionNumber())) {
        dsyslog ("vdrtva: SDT Syncer error\n");
        return;
      }
      SI::SDT::Service SiSdtService;
      for (SI::Loop::Iterator it; sdt.serviceLoop.getNext(SiSdtService, it); ) {
	cChannel *chan = Channels.GetByChannelID(tChannelID(Source(),sdt.getOriginalNetworkId(),sdt.getTransportStreamId(),SiSdtService.getServiceId()));
	if (chan) {
	  cChanDA *chanDA = ChanDAs->GetByChannelID(chan->Number());
	  if (!chanDA) {
	    SI::Descriptor *d;
	    for (SI::Loop::Iterator it2; (d = SiSdtService.serviceDescriptors.getNext(it2)); ) {
	      switch (d->getDescriptorTag()) {
		case SI::DefaultAuthorityDescriptorTag: {
		  SI::DefaultAuthorityDescriptor *da = (SI::DefaultAuthorityDescriptor *)d;
		  char DaBuf[Utf8BufSize(1024)];
		  da->DefaultAuthority.getText(DaBuf, sizeof(DaBuf));
		  chanDA = ChanDAs->NewChanDA(chan->Number());
		  chanDA->SetDA(DaBuf);
		}
		break;
	      default: ;
	      }
	      delete d;
	    }
	  }
        }
      }
    }
    case 0x12: {
      if (Tid >= 0x4E && Tid <= 0x6F) {
//      sectionSyncer.Reset();
        SI::EIT eit(Data, false);
        if (!eit.CheckCRCAndParse()) {
          dsyslog ("vdrtva: EIT Parse / CRC error\n");
          return;
        }

	cChannel *chan = Channels.GetByChannelID(tChannelID(Source(),eit.getOriginalNetworkId(),eit.getTransportStreamId(),eit.getServiceId()));
	if (!chan) {
	  return;
	}
	SI::EIT::Event SiEitEvent;
        for (SI::Loop::Iterator it; eit.eventLoop.getNext(SiEitEvent, it); ) {
          cEventCRID *eventCRID = EventCRIDs->GetByID(chan->Number(), SiEitEvent.getEventId());
          if (!eventCRID) {
            SI::Descriptor *d;
            char iCRIDBuf[Utf8BufSize(256)] = {'\0'}, sCRIDBuf[Utf8BufSize(256)] = {'\0'}, gCRIDBuf[Utf8BufSize(256)] = {'\0'};
            for (SI::Loop::Iterator it2; (d = SiEitEvent.eventDescriptors.getNext(it2)); ) {
              switch (d->getDescriptorTag()) {
                case SI::ContentIdentifierDescriptorTag: {
                  SI::ContentIdentifierDescriptor *cd = (SI::ContentIdentifierDescriptor *)d;
                  SI::ContentIdentifierDescriptor::Identifier cde;
                  for (SI::Loop::Iterator ite; (cd->identifierLoop.getNext(cde,ite)); ) {
                    if (cde.getCridLocation() == 0) {
		      switch (cde.getCridType()) {
		        case 0x01:	// ETSI 102 323 code
		        case 0x31:	// UK Freeview private code
			  cde.identifier.getText(iCRIDBuf, sizeof(iCRIDBuf));
			  break;
		        case 0x02:	// ETSI 102 323 code
		        case 0x32:	// UK Freeview private code
			  cde.identifier.getText(sCRIDBuf, sizeof(sCRIDBuf));
			  break;
			// ETSI 102 323 defines CRID type 0x03, which describes 'related' or 'suggested' events.
			// Freeview broadcasts these as CRID type 0x33.
			// There can be more than one type 0x33 descriptor per event (each with one CRID).
			case 0x03:
			case 0x33:
			  cde.identifier.getText(gCRIDBuf, sizeof(gCRIDBuf));		// FIXME Rashly assuming that a 0x31 CRID will always precede a 0x33 CRID.
			  if (iCRIDBuf[0]) SuggestCRIDs->NewSuggestCRID(chan->Number(), iCRIDBuf, gCRIDBuf);
		      }
                    }
                    else {
                      dsyslog ("vdrtva: Incorrect CRID Loc %x\n", cde.getCridLocation());
                    } 
                  }
                }
                break;
                default: ;
              }
              delete d;
            }
            if (iCRIDBuf[0] && sCRIDBuf[0]) {	// Only log events which are part of a series.
              eventCRID = EventCRIDs->NewEventCRID(chan->Number(), SiEitEvent.getEventId());
	      eventCRID->SetCRIDs(iCRIDBuf, sCRIDBuf);
            }
	  }
        }
      }
    }
    break;
  }
}


/*
  cChanDA - Default Authority for a channel.
*/

cChanDA::cChanDA(void)
{
  defaultAuthority = NULL;
}

cChanDA::~cChanDA(void)
{
  free(defaultAuthority);
}


void cChanDA::Set(int Cid) {
  cid = Cid; 
}

void cChanDA::SetDA(char *DA) {
  defaultAuthority = strcpyrealloc(defaultAuthority, DA);
}

/*
  cChanDAs - in-memory list of channels and Default Authorities.
*/

cChanDAs::cChanDAs(void)
{
  maxNumber = 0;
}

cChanDAs::~cChanDAs(void)
{
  chanDAHash.Clear();
}

cChanDA *cChanDAs::GetByChannelID(int cid)
{
  cList<cHashObject> *list = chanDAHash.GetList(cid);
  if (list) {
     for (cHashObject *hobj = list->First(); hobj; hobj = list->Next(hobj)) {
       cChanDA *chanDA = (cChanDA *)hobj->Object();
       if (chanDA->Cid() == cid)
            return chanDA;
     }
   }
  return NULL;
}

cChanDA *cChanDAs::NewChanDA(int Cid)
{
  cChanDA *NewChanDA = new cChanDA;
  NewChanDA->Set(Cid);
  Add(NewChanDA);
  chanDAHash.Add(NewChanDA, Cid);
  ChanDAs->SetMaxNumber(ChanDAs->MaxNumber()+1);
  return NewChanDA;
}


/*
  cEventCRID - CRIDs for an event.
*/

cEventCRID::cEventCRID(void)
{
  iCrid = sCrid = NULL;
}

cEventCRID::~cEventCRID(void)
{
  free (iCrid);
  free (sCrid);
}

void cEventCRID::Set(int Cid, tEventID Eid) {
  eid = Eid; 
  cid = Cid; 
}

void cEventCRID::SetCRIDs(char *iCRID, char *sCRID) {
  iCrid = strcpyrealloc(iCrid, iCRID);
  sCrid = strcpyrealloc(sCrid, sCRID);
}


/*
  cEventCRIDs - in-memory list of events and CRIDs.
*/

cEventCRIDs::cEventCRIDs(void)
{
  maxNumber = 0;
}

cEventCRIDs::~cEventCRIDs(void)
{
  EventCRIDHash.Clear();
}

cEventCRID *cEventCRIDs::GetByID(int Cid, tEventID Eid)
{
  cList<cHashObject> *list = EventCRIDHash.GetList(Cid*33000 + Eid);
  if (list) {
     for (cHashObject *hobj = list->First(); hobj; hobj = list->Next(hobj)) {
       cEventCRID *EventCRID = (cEventCRID *)hobj->Object();
       if ((EventCRID->Eid() == Eid) && (EventCRID->Cid() == Cid))
            return EventCRID;
     }
   }
  return NULL;
}

cEventCRID *cEventCRIDs::NewEventCRID(int Cid, tEventID Eid)
{
  cEventCRID *NewEventCRID = new cEventCRID;
  NewEventCRID->Set(Cid, Eid);
  Add(NewEventCRID);
  EventCRIDHash.Add(NewEventCRID, Eid + Cid*33000);
  EventCRIDs->SetMaxNumber(EventCRIDs->MaxNumber()+1);
  return NewEventCRID;
}


/*
  cSuggestCRID - CRIDs of suggested items for an event.
*/

cSuggestCRID::cSuggestCRID(void)
{
  iCrid = gCrid = NULL;
}

cSuggestCRID::~cSuggestCRID(void)
{
  free (iCrid);
  free (gCrid);
}

void cSuggestCRID::Set(int Cid, char *iCRID, char *gCRID) {
  iCrid = strcpyrealloc(iCrid, iCRID);
  gCrid = strcpyrealloc(gCrid, gCRID);
  cid = Cid;
}

int cSuggestCRID::Compare(const cListObject &ListObject) const
{
  cSuggestCRID *s = (cSuggestCRID *) &ListObject;
  if (int r = cid - s->Cid()) return r;
  if (int r = strcmp(iCrid, s->iCRID())) return r;
  if (int r = strcmp(gCrid, s->gCRID())) return r;
  return 0;
}


/*
  cSuggestCRIDs - in-memory list of suggested events
*/

cSuggestCRIDs::cSuggestCRIDs(void)
{
  maxNumber = 0;
}

cSuggestCRIDs::~cSuggestCRIDs(void)
{
}

cSuggestCRID *cSuggestCRIDs::NewSuggestCRID(int cid, char *icrid, char *gcrid)
{
  cSuggestCRID *NewSuggestCRID = new cSuggestCRID;
  NewSuggestCRID->Set(cid, icrid, gcrid);
  Add(NewSuggestCRID);
  SuggestCRIDs->SetMaxNumber(SuggestCRIDs->MaxNumber()+1);
  return NewSuggestCRID;
}


/*
	cLinkItem - Entry from the links file
*/

cLinkItem::cLinkItem(void)
{
  sCrid = iCrids = NULL;
}

cLinkItem::~cLinkItem(void)
{
  free(sCrid);
  free(iCrids);
}

void cLinkItem::Set(const char *sCRID, int ModTime, const char *iCRIDs)
{
  sCrid = strcpyrealloc(sCrid, sCRID);
  modtime = ModTime;
  iCrids = strcpyrealloc(iCrids, iCRIDs);
}

/*
	cLinks - list of cLinkItem entities
*/

cLinks::cLinks(void)
{
  maxNumber = 0;
}

cLinkItem *cLinks::NewLinkItem(const char *sCRID, int ModTime, const char *iCRIDs)
{
  cLinkItem *NewLinkItem = new cLinkItem;
  NewLinkItem->Set(sCRID, ModTime, iCRIDs);
  Add(NewLinkItem);
  Links->SetMaxNumber(Links->MaxNumber()+1);
  return NewLinkItem;
}

VDRPLUGINCREATOR(cPluginvdrTva); // Don't touch this!
