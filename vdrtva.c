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


cChanDAs ChanDAs;
cEventCRIDs EventCRIDs;
cSuggestCRIDs SuggestCRIDs;
cLinks Links;
cTvaLog tvalog;
char *configDir;

static const char *VERSION        = "0.3.6";
static const char *DESCRIPTION    = "Series Record plugin";
static const char *MAINMENUENTRY  = "Series Links";

int collectionperiod;		// Time to collect all CRID data (secs, default 600)
int lifetime;			// Lifetime of series link recordings (default 99)
int priority;			// Priority of series link recordings (default 99)
int seriesLifetime;		// Expiry time of a series link (default 91 days)
int updatetime;			// Time to carry out the series link update HHMM (default 03:00)
bool checkCollisions;		// Whether to test for collisions (assuming single DVB card)
bool captureComplete;		// Flag set if initial CRID capture has completed.

class cPluginvdrTva : public cPlugin {
private:
  // Add any member variables or functions you may need here.
  time_t nextactiontime;
  cTvaFilter *Filter;
  cTvaStatusMonitor *statusMonitor;
  bool AppendItems(const char* Option);
  void UpdateLinksFromTimers(void);
  void AddNewEventsToSeries(void);
  bool CheckSplitTimers(void);
  bool CreateTimerFromEvent(const cEvent *event, char *Path);
  void CheckChangedEvents(void);
  void CheckTimerClashes(void);
  void FindAlternatives(const cEvent *event);
  void FindSuggestions(const cEvent *event);
  void StartDataCapture(void);
  void StopDataCapture(void);
  void Update(void);
  void Check(bool daily);
  void Report(void);
  void Expire(void);
  void tvasyslog(const char *Fmt, ...);
  time_t NextUpdateTime(void);
  cTimeMs capture;

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
  virtual const char *MainMenuEntry(void) { return tr(MAINMENUENTRY); }
  virtual cOsdObject *MainMenuAction(void);
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
  seriesLifetime = 91 * SECSINDAY;
  priority = 99;
  lifetime = 99;
  collectionperiod = 10 * 60;	//secs
  updatetime = 300;
  captureComplete = false;
  checkCollisions = true;
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
	 "  -n       --nocheck          Do not check for timer collisions\n"
	 "  -p n     --priority=n       Priority of new timers (default 99)\n"
	 "  -s n     --serieslifetime=n Days to remember a series after the last event\n"
	 "                              (default 91)\n"
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
       { "nocheck",	   no_argument,       NULL, 'n' },
       { NULL }
     };

  int c, opt;
  char *hours, *mins, *strtok_next;
  char buf[32];
  while ((c = getopt_long(argc, argv, "l:m:n:p:s:u:", long_options, NULL)) != -1) {
    switch (c) {
      case 'l':
	opt = atoi(optarg);
	if (opt > 0) lifetime = opt;
	break;
      case 'm':
	tvalog.setmailTo(optarg);
	break;
      case 'n':
	checkCollisions = false;
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
  Links.Load();
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
  capture.Set(collectionperiod * 1000);
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
  Links.Save();
}

void cPluginvdrTva::Housekeeping(void)
{
  // Perform any cleanup or other regular tasks.
  static int state = 0;

  if (captureComplete && (nextactiontime < time(NULL))) {
    statusMonitor->ClearTimerAdded();		// Ignore any timer changes while update is in progress
    switch (state) {
      case 0:
	Expire();
	Update();
	state++;
	break;
      case 1:
	Check(1);
	Report();
	nextactiontime = NextUpdateTime();
	state = 0;
	tvalog.MailLog();
	break;
    }
  }
  else if (statusMonitor->GetTimerAddedDelta() > 60) {
    Update();			// Wait 1 minute for VDR to enter the event data into the new timer.
    Check(0);
    statusMonitor->ClearTimerAdded();
  }
}

time_t cPluginvdrTva::NextUpdateTime(void)
{
  struct tm tm_r;
  time_t now, then;
  char buff[32];

  now = time(NULL);
  localtime_r(&now, &tm_r);
  tm_r.tm_sec = 0;
  tm_r.tm_hour = updatetime / 100;
  tm_r.tm_min = updatetime % 100;
  tm_r.tm_mday++;
  then = mktime(&tm_r);
  if (then < now) then += SECSINDAY;
  ctime_r(&then, buff);
  isyslog("vdrtva: Next update due at %s", buff);
  return then;
}

void cPluginvdrTva::MainThreadHook(void)
{
  // Perform actions in the context of the main program thread.
  // WARNING: Use with great care - see PLUGINS.html!
  
  static bool running = false;

  if (!running && (capture.Elapsed() > 5000)) {
    StartDataCapture();
    running = true;
  }
  if (!captureComplete && capture.TimedOut()) {
    captureComplete = true;
    nextactiontime = time(NULL) + 1;
  }
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

cOsdObject *cPluginvdrTva::MainMenuAction(void)
{
  // Perform the action when selected from the main VDR menu.
  return new cMenuLinks;
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
  else if (!strcasecmp(Name, "CheckCollisions")) checkCollisions = atoi(Value);
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
    "DELL <sCrid>\n"
    "    Delete series link by series CRID",
    "LLOG\n"
    "    Print the action log.",
    "LSTL\n"
    "    Print the Links list.",
    "LSTS\n"
    "    Print the suggested events list",
    "LSTT\n"
    "    Print the list of timers with suggestions for each event",
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
  if (strcasecmp(Command, "DELL") == 0) {
    if (!captureComplete) return cString::sprintf("Data capture still in progress");
    if (Links.DeleteItem(Option)) {
      return cString::sprintf("Series %s deleted", Option);
    }
    else return cString::sprintf("Series %s not found in links file", Option);
  }
  else if (strcasecmp(Command, "LLOG") == 0) {
    ReplyCode = 250;
    if (tvalog.Length() > 0) return cString(tvalog.Buffer());
    else return cString::sprintf("Nothing in the buffer!");
  }
  else if (strcasecmp(Command, "LSTL") == 0) {
    if (Links.MaxNumber() >=1) {
      ReplyCode = 250;
      for (cLinkItem *linkItem = Links.First(); linkItem; linkItem = Links.Next(linkItem)) {
	reply.Append("%s;%d;%s;%s;%s;%s\n", linkItem->sCRID(), linkItem->ModTime(), linkItem->iCRIDs(), linkItem->Path(), linkItem->Title(), linkItem->channelName());
      }
    }
    if (reply.Length() > 0) return cString(reply.Buffer());
    else return cString::sprintf("Nothing in the buffer!");
  }
  else if (strcasecmp(Command, "LSTS") == 0) {
    if (!captureComplete) return cString::sprintf("Data capture still in progress");
    if (SuggestCRIDs.MaxNumber() >= 1) {
      ReplyCode = 250;
      cSuggestCRID *suggest = SuggestCRIDs.First();
      while (suggest) {
	cSuggestCRID *next = SuggestCRIDs.Next(suggest);
	cChanDA *chanDA = ChanDAs.GetByChannelID(suggest->Cid());
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
  else if (strcasecmp(Command, "LSTT") == 0) {
    if (Timers.Count() == 0) return cString::sprintf("No timers defined");
    Report();
    return cString::sprintf("Report generated");
  }
  else if (strcasecmp(Command, "LSTY") == 0) {
    if (EventCRIDs.MaxNumber() >= 1) {
       ReplyCode = 250;
       for (cEventCRID *eventCRID = EventCRIDs.First(); eventCRID; eventCRID = EventCRIDs.Next(eventCRID)) {
	  cChanDA *chanDA = ChanDAs.GetByChannelID(eventCRID->Cid());
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
    if (ChanDAs.MaxNumber() >= 1) {
       ReplyCode = 250;
       for (cChanDA *chanDA = ChanDAs.First(); chanDA; chanDA = ChanDAs.Next(chanDA)) {
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
    if (captureComplete) {
      Update();
      Check(0);
      return cString::sprintf("Update completed");
    }
    else {
      ReplyCode = 999;
      return cString::sprintf("Data capture in progress");
    }
  }
  return NULL;
}

void cPluginvdrTva::StartDataCapture()
{
  if (!Filter) {
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
    isyslog("vdrtva: Data capture stopped");
  }
}

void cPluginvdrTva::Expire()
{
  EventCRIDs.Expire();
  SuggestCRIDs.DeDup();
  SuggestCRIDs.Expire();
  Links.Expire();
  Links.Save();
}

void cPluginvdrTva::Update()
{
  UpdateLinksFromTimers();
  AddNewEventsToSeries();
  Links.Save();
  isyslog("vdrtva: Updates complete");
}

void cPluginvdrTva::Check(bool daily)
{
  if (daily) {
    REPORT(" \nDaily Timer Check\n-----------------\n ");
  }
  CheckChangedEvents();
  if (checkCollisions) {
    CheckTimerClashes();
  }
  CheckSplitTimers();
  isyslog("vdrtva: Checks complete");
}

void cPluginvdrTva::Report()
{
  if ((Timers.Count() == 0) || (!captureComplete)) return;
  REPORT(" \nTimers and Suggestions\n----------------------\n ");
  cSortedTimers SortedTimers;
  for (int i = 0; i < SortedTimers.Size(); i++) {
    if (const cTimer *ti = SortedTimers[i]) {
      const cEvent *event = ti->Event();
      if (event && ti->HasFlags(tfActive)) {
        REPORT("'%s' (%s %s)", event->Title(), ti->Channel()->Name(), *DayDateTime(event->StartTime()));
        FindSuggestions(event);
      }
    }
  }
}

// Check that all timers are part of series links and update the links.

void cPluginvdrTva::UpdateLinksFromTimers()
{
  if ((Timers.Count() == 0) || (!captureComplete)) return;
  for (cTimer *ti = Timers.First(); ti; ti = Timers.Next(ti)) {
// find the event for this timer
    const cEvent *event = ti->Event();
    if (event && ti->HasFlags(tfActive) && (ti->WeekDays() == 0)) {
      cChannel *channel = Channels.GetByChannelID(event->ChannelID());
// find the sCRID and iCRID for the event
      cChanDA *chanda = ChanDAs.GetByChannelID(channel->Number());
      cEventCRID *eventcrid = EventCRIDs.GetByID(channel->Number(), event->EventID());
      if (eventcrid && chanda) {
	cString scrid = cString::sprintf("%s%s", chanda->DA(),eventcrid->sCRID());
	cString icrid = cString::sprintf("%s%s", chanda->DA(),eventcrid->iCRID());
// scan the links table for the sCRID
//   if found, check if the iCRID is present, if not add it
//   else create a new links entry
	char *path = strcpyrealloc(NULL, ti->File());
	if (char *p = strrchr(path, '~')) {
	  *p = '\0';
	  p++;
	  Links.AddSeriesLink(scrid, event->StartTime(), icrid, path, p, channel->Name());
	}
	else Links.AddSeriesLink(scrid, event->StartTime(), icrid, NULL, path, channel->Name());
	free (path);
      }
    }
  }
}

void cPluginvdrTva::AddNewEventsToSeries()
{
  if (Links.MaxNumber() < 1) return;
  cSchedulesLock SchedulesLock;
  const cSchedules *Schedules = cSchedules::Schedules(SchedulesLock);
  if (!Schedules) return;
// Foreach CRID
  for (cEventCRID *eventCRID = EventCRIDs.First(); eventCRID; eventCRID = EventCRIDs.Next(eventCRID)) {
    cChanDA *chanDA = ChanDAs.GetByChannelID(eventCRID->Cid());
    if (chanDA) {
// Do we have a series link for this sCRID?
      cString scrid = cString::sprintf("%s%s", chanDA->DA(),eventCRID->sCRID());
      cLinkItem *Item = Links.getLinkItem(scrid);
      if (Item != NULL) {
// Is the event already being recorded, possibly as part of a different series?
	cString icrid = cString::sprintf("%s%s", chanDA->DA(),eventCRID->iCRID());
	if (Links.isEventNeeded(icrid)) {
// Is the event on the same channel as the first event of the series? If so create a new timer.
	  cChannel *channel = Channels.GetByNumber(eventCRID->Cid());
	  const cSchedule *schedule = Schedules->GetSchedule(channel);
	  if (schedule && (!Item->channelName() || !strcmp(channel->Name(), Item->channelName()))) {
	    const cEvent *event = schedule->GetEvent(eventCRID->Eid());
	    if (CreateTimerFromEvent(event, Item->Path())) {
	      Links.AddSeriesLink(scrid, event->StartTime(), icrid, NULL, NULL, NULL);
	    }
	  }
	}
      }
    }
  }
}

// Check timers to see if the event they were set to record is still in the EPG.
// This won't work if VPS is not used and the start time is padded by a custom amount.
// TODO Go hunting for another instance of the event.

void cPluginvdrTva::CheckChangedEvents()
{
  cSchedulesLock SchedulesLock;
  const cSchedules *Schedules = cSchedules::Schedules(SchedulesLock);
  if (Timers.Count() == 0) return;
  for (cTimer *ti = Timers.First(); ti; ti = Timers.Next(ti)) {
    const cChannel *channel = ti->Channel();
    const cSchedule *schedule = Schedules->GetSchedule(channel);
    if (schedule && ti->HasFlags(tfActive)) {
      time_t start_time = ti->StartTime();
      if (!ti->HasFlags(tfVps)) {
	start_time += Setup.MarginStart * 60;
      }
      const char *file = strrchr(ti->File(), '~');
      if (!file) file = ti->File();
      else file++;
      const cEvent *event = schedule->GetEvent(0, start_time);
      if (!event) REPORT("Event for timer '%s' at %s seems to no longer exist", file, *DayDateTime(ti->StartTime()));
      else if (strcmp(file, event->Title())) {
	REPORT("Changed timer event at %s: %s <=> %s", *DayDateTime(ti->StartTime()), file, event->Title());
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
    if (timer1 && timer1->HasFlags(tfActive)) {
      for (int j = 0; j < i; j++) {
	cTimer *timer2 = Timers.Get(j);
	if (timer2 && timer2->HasFlags(tfActive)) {
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
  cChanDA *chanda = ChanDAs.GetByChannelID(channel->Number());
  cEventCRID *eventcrid = EventCRIDs.GetByID(channel->Number(), event->EventID());
  cSchedulesLock SchedulesLock;
  const cSchedules *schedules = cSchedules::Schedules(SchedulesLock);
  if (!eventcrid || !chanda) {
    REPORT("Cannot find alternatives for '%s' - not part of a series", event->Title());
    return;
  }
  bool found = false;
  for (cEventCRID *eventcrid2 = EventCRIDs.First(); eventcrid2; eventcrid2 = EventCRIDs.Next(eventcrid2)) {
    if ((strcmp(eventcrid->iCRID(), eventcrid2->iCRID()) == 0) && (event->EventID() != eventcrid2->Eid())) {
      cChanDA *chanda2 = ChanDAs.GetByChannelID(eventcrid2->Cid());
      if (strcmp(chanda->DA(), chanda2->DA()) == 0) {
	cChannel *channel2 = Channels.GetByNumber(eventcrid2->Cid());
	const cSchedule *schedule = schedules->GetSchedule(channel2);
	if (schedule) {
	  const cEvent *event2 = schedule->GetEvent(eventcrid2->Eid(), 0);
	  if (event2) {
	    if (!found) {
	      REPORT("Alternatives for '%s':", event->Title());
	      found = true;
	    }
	    bool clash = false;
	    for (cTimer *ti = Timers.First(); ti; ti = Timers.Next(ti)) {
	      if((ti->StartTime() >= event2->StartTime() && ti->StartTime() < event2->EndTime())
	      ||(event2->StartTime() >= ti->StartTime() && event2->StartTime() < ti->StopTime())) {
		if (ti->Channel()->Transponder() != channel2->Transponder()) {
		  const char *file = strrchr(ti->File(), '~');
		  if (!file) file = ti->File();
		  else file++;
		  REPORT("  %s %s (clash with timer '%s')", channel2->Name(), *DayDateTime(event2->StartTime()), file);
		  clash = true;
		}
	      }
	    }
	    if (!clash) {
	      REPORT("  %s %s", channel2->Name(), *DayDateTime(event2->StartTime()));
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
  for (cTimer *ti = Timers.First(); ti; ti = Timers.Next(ti)) {
    const cEvent *event = ti->Event();
    if (event && ti->HasFlags(tfActive)) {
      cChannel *channel = Channels.GetByChannelID(event->ChannelID());
      cChanDA *chanda = ChanDAs.GetByChannelID(channel->Number());
      cEventCRID *eventcrid = EventCRIDs.GetByID(channel->Number(), event->EventID());
      if (eventcrid && chanda && strchr(eventcrid->iCRID(), '#')) {
//	  char crid[Utf8BufSize(256)], *next;
//	  strcpy(crid, eventcrid->iCRID());
//	  char *prefix = strtok_r(crid, "#", &next);
//	  char *suffix = strtok_r(NULL, "#", &next);
	REPORT("Timer for split event '%s' found - check all parts are being recorded!", event->Title());
      }
    }
  }
  return false;
}

// Create a timer from an event, setting VPS parameter explicitly.

bool cPluginvdrTva::CreateTimerFromEvent(const cEvent *event, char *Path) {
  if (!event) {
    dsyslog("vdrtva: CreateTimerFromEvent() called without Event!");
    return false;
  }
  struct tm tm_r;
  char startbuff[64], endbuff[64], etitle[256];
  int flags = tfActive;
  cChannel *channel = Channels.GetByChannelID(event->ChannelID());
  time_t starttime = event->StartTime();
  time_t endtime = event->EndTime();
  if (!Setup.UseVps) {
    starttime -= Setup.MarginStart * 60;
    endtime += Setup.MarginStop * 60;
  }
  else flags |= tfVps;
  localtime_r(&starttime, &tm_r);
  strftime(startbuff, sizeof(startbuff), "%Y-%m-%d:%H%M", &tm_r);
  localtime_r(&endtime, &tm_r);
  strftime(endbuff, sizeof(endbuff), "%H%M", &tm_r);
  strn0cpy (etitle, event->Title(), sizeof(etitle));
  strreplace(etitle, ':', '|');
  cString timercmd;
  if (Path) {
    timercmd = cString::sprintf("%u:%d:%s:%s:%d:%d:%s~%s:\n", flags, channel->Number(), startbuff, endbuff, priority, lifetime, Path, etitle);
  }
  else {
    timercmd = cString::sprintf("%u:%d:%s:%s:%d:%d:%s:\n", flags, channel->Number(), startbuff, endbuff, priority, lifetime, etitle);
  }
  cTimer *timer = new cTimer;
  if (timer->Parse(timercmd)) {
    cTimer *t = Timers.GetTimer(timer);
    if (!t) {
      timer->SetEvent(event);
      Timers.Add(timer);
      Timers.SetModified();
      REPORT("Timer created for '%s' on %s, %s %04d-%04d", event->Title(), channel->Name(), *DateString(starttime), timer->Start(), timer->Stop());
      return true;
    }
    isyslog("vdrtva: Duplicate timer creation attempted for %s on %s", *timer->ToDescr(), *DateString(timer->StartTime()));
  }
  return false;
}

//	Find 'suggestions' for an event

void cPluginvdrTva::FindSuggestions(const cEvent *event)
{
  bool found = false;
  cChannel *channel = Channels.GetByChannelID(event->ChannelID());
  cChanDA *chanda = ChanDAs.GetByChannelID(channel->Number());
  cEventCRID *eventcrid = EventCRIDs.GetByID(channel->Number(), event->EventID());
  cSchedulesLock SchedulesLock;
  const cSchedules *schedules = cSchedules::Schedules(SchedulesLock);
  if (eventcrid && chanda && schedules) {
    for (cSuggestCRID *suggestcrid = SuggestCRIDs.First(); suggestcrid; suggestcrid = SuggestCRIDs.Next(suggestcrid)) {
      if((channel->Number() == suggestcrid->Cid()) && (!strcmp(suggestcrid->iCRID(), eventcrid->iCRID()))) {
	for (cEventCRID *ecrid2 = EventCRIDs.First(); ecrid2; ecrid2 = EventCRIDs.Next(ecrid2)) {
	  if (!strcmp(suggestcrid->gCRID(), ecrid2->iCRID())) {
	    cChanDA *chanda2 = ChanDAs.GetByChannelID(ecrid2->Cid());
	    if (!strcmp(chanda->DA(), chanda2->DA())) {
	      cChannel *channel2 = Channels.GetByNumber(ecrid2->Cid());
	      const cSchedule *schedule = schedules->GetSchedule(channel2);
	      if (schedule) {
		const cEvent *event2 = schedule->GetEvent(ecrid2->Eid(), 0);
		if (!found) {
		  REPORT("  Suggestions for this event:");
		  found = true;
		}
		REPORT("    '%s' (%s, %s)", event2->Title(), channel2->Name(), *DayDateTime(event2->StartTime()));
	      }
	    }
	  }
	}
      }
    }
  }
}

//	Report actions to syslog if we don't want an email.

void cPluginvdrTva::tvasyslog(const char *Fmt, ...) {
  
  va_list ap;
  int size = 4096;
  char *buff = (char *) malloc(sizeof(char) * size);

  while (buff) {
    va_start(ap, Fmt);
    int n = vsnprintf(buff, size, Fmt, ap);
    va_end(ap);
    if (n < size) {
      char *save, *b = buff;
      while (b = strtok_r(b, "\n", &save)) {
        isyslog("vdrtva: %s", b);
	b = NULL;
      }
      free(buff);
      return;
    }
// overflow: realloc and try again
    size *= 2;
    char *tmp = (char *) realloc(buff, sizeof(char) * size);
    if (!tmp) free(buff);
    buff = tmp;
  }
}



/*
	cTvaStatusMonitor - callback for timer changes.
*/

cTvaStatusMonitor::cTvaStatusMonitor(void)
{
  timeradded = 0;
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
  timeradded = 0;
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
  newcheckcollisions = checkCollisions;
  Add(new cMenuEditIntItem(tr("Collection period (min)"), &newcollectionperiod, 1, 99));
  Add(new cMenuEditIntItem(tr("Series link lifetime (days)"), &newseriesLifetime, 1, 366));
  Add(new cMenuEditIntItem(tr("New timer lifetime"), &newlifetime, 0, 99));
  Add(new cMenuEditIntItem(tr("New timer priority"), &newpriority, 0, 99));
  Add(new cMenuEditTimeItem(tr("Update Time (HH:MM)"), &newupdatetime));
  Add(new cMenuEditBoolItem(tr("Check collisions"), &newcheckcollisions));
}

void cTvaMenuSetup::Store(void)
{
  SetupStore("CollectionPeriod", newcollectionperiod); collectionperiod = newcollectionperiod * 60;
  SetupStore("SeriesLifetime", newseriesLifetime); seriesLifetime = newseriesLifetime * SECSINDAY;
  SetupStore("TimerLifetime", newlifetime); lifetime = newlifetime;
  SetupStore("TimerPriority", newpriority); priority = newpriority;
  SetupStore("UpdateTime", newupdatetime); updatetime = newupdatetime;
  SetupStore("CheckCollisions", newcheckcollisions); checkCollisions = newcheckcollisions;
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
  fprintf(mail, "Content-Type: text/plain; charset=ISO-8859-1\n\n");
  fprintf(mail, "Activity since last report\n--------------------------\n \n");
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
        return;
      }
      if (!sectionSyncer.Sync(sdt.getVersionNumber(), sdt.getSectionNumber(), sdt.getLastSectionNumber())) {
        return;
      }
      SI::SDT::Service SiSdtService;
      for (SI::Loop::Iterator it; sdt.serviceLoop.getNext(SiSdtService, it); ) {
	cChannel *chan = Channels.GetByChannelID(tChannelID(Source(),sdt.getOriginalNetworkId(),sdt.getTransportStreamId(),SiSdtService.getServiceId()));
	if (chan) {
	  cChanDA *chanDA = ChanDAs.GetByChannelID(chan->Number());
	  if (!chanDA) {
	    SI::Descriptor *d;
	    for (SI::Loop::Iterator it2; (d = SiSdtService.serviceDescriptors.getNext(it2)); ) {
	      switch (d->getDescriptorTag()) {
		case SI::DefaultAuthorityDescriptorTag: {
		  SI::DefaultAuthorityDescriptor *da = (SI::DefaultAuthorityDescriptor *)d;
		  char DaBuf[Utf8BufSize(1024)];
		  da->DefaultAuthority.getText(DaBuf, sizeof(DaBuf));
		  chanDA = ChanDAs.NewChanDA(chan->Number(), DaBuf);
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
          return;
        }

	cChannel *chan = Channels.GetByChannelID(tChannelID(Source(),eit.getOriginalNetworkId(),eit.getTransportStreamId(),eit.getServiceId()));
	if (!chan) {
	  return;
	}
	SI::EIT::Event SiEitEvent;
        for (SI::Loop::Iterator it; eit.eventLoop.getNext(SiEitEvent, it); ) {
          cEventCRID *eventCRID = EventCRIDs.GetByID(chan->Number(), SiEitEvent.getEventId());
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
			  cde.identifier.getText(gCRIDBuf, sizeof(gCRIDBuf));		// FIXME Rashly assuming that 0x31 & 0x32 CRIDs will always precede a 0x33 CRID.
			  if (iCRIDBuf[0] && sCRIDBuf[0]) SuggestCRIDs.NewSuggestCRID(chan->Number(), iCRIDBuf, gCRIDBuf);
		      }
                    }
                    else {
                      dsyslog ("vdrtva: Incorrect CRID Loc %x", cde.getCridLocation());
                    } 
                  }
                }
                break;
                default: ;
              }
              delete d;
            }
            if (iCRIDBuf[0] && sCRIDBuf[0]) {	// Only log events which are part of a series.
              eventCRID = EventCRIDs.NewEventCRID(chan->Number(), SiEitEvent.getEventId(), iCRIDBuf, sCRIDBuf);
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

cChanDA::cChanDA(int Cid, char *DA)
{
  cid = Cid; 
  if (startswith(DA, "crid://")) defaultAuthority = strcpyrealloc(NULL, &DA[7]);
  else defaultAuthority = strcpyrealloc(NULL, DA);
}

cChanDA::~cChanDA(void)
{
  free(defaultAuthority);
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

cChanDA *cChanDAs::NewChanDA(int Cid, char *DA)
{
  cChanDA *NewChanDA = new cChanDA(Cid, DA);
  Add(NewChanDA);
  chanDAHash.Add(NewChanDA, Cid);
  maxNumber++;
  return NewChanDA;
}


/*
  cEventCRID - CRIDs for an event.
*/

cEventCRID::cEventCRID(int Cid, tEventID Eid, char *iCRID, char *sCRID)
{
  eid = Eid; 
  cid = Cid; 
  iCrid = strcpyrealloc(NULL, iCRID);
  sCrid = strcpyrealloc(NULL, sCRID);
}

cEventCRID::~cEventCRID(void)
{
  free (iCrid);
  free (sCrid);
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

cEventCRID *cEventCRIDs::NewEventCRID(int Cid, tEventID Eid, char *iCRID, char *sCRID)
{
  cEventCRID *NewEventCRID = new cEventCRID(Cid, Eid, iCRID, sCRID);
  Add(NewEventCRID);
  EventCRIDHash.Add(NewEventCRID, Eid + Cid*33000);
  maxNumber++;
  return NewEventCRID;
}

void cEventCRIDs::Expire(void)
{
  int i = 0;
  cSchedulesLock SchedulesLock;
  const cSchedules *schedules = cSchedules::Schedules(SchedulesLock);
  if (schedules) {
    cEventCRID *crid = First();
    while (crid) {
      cEventCRID *next = Next(crid);
      cChannel *channel = Channels.GetByNumber(crid->Cid());
      const cSchedule *schedule = schedules->GetSchedule(channel);
      if (schedule) {
	const cEvent *event = schedule->GetEvent(crid->Eid(), 0);
	if (!event) {
	  Del(crid);
	  maxNumber--;
	  i++;
	}
      }
      crid = next;
    }
  }
  dsyslog("vdrtva: %d expired CRIDs removed", i);
}


/*
  cSuggestCRID - CRIDs of suggested items for an event.
*/

cSuggestCRID::cSuggestCRID(int Cid, char *iCRID, char *gCRID)
{
  iCrid = strcpyrealloc(NULL, iCRID);
  gCrid = strcpyrealloc(NULL, gCRID);
  cid = Cid;
}

cSuggestCRID::~cSuggestCRID(void)
{
  free (iCrid);
  free (gCrid);
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

cSuggestCRID *cSuggestCRIDs::NewSuggestCRID(int cid, char *icrid, char *gcrid)
{
  cSuggestCRID *NewSuggestCRID = new cSuggestCRID(cid, icrid, gcrid);
  Add(NewSuggestCRID);
  maxNumber++;
  return NewSuggestCRID;
}

void cSuggestCRIDs::DeDup(void) {
  if (maxNumber < 2) return;
  int i = 0;
  Sort();
  cSuggestCRID *suggest = First();
  while (suggest) {
    cSuggestCRID *next = Next(suggest);
    if (next && !strcmp(next->iCRID(), suggest->iCRID()) && !strcmp(next->gCRID(), suggest->gCRID())) {
      Del(suggest);
      maxNumber--;
      i++;
    }
    suggest = next;
  }
  dsyslog("vdrtva: %d duplicate suggestions removed", i);
}

void cSuggestCRIDs::Expire(void) {
  if (maxNumber == 0) return;
  int i = 0;
  cSuggestCRID *suggest = First();
  while (suggest) {
    cSuggestCRID *next = Next(suggest);
    bool found = false;
    for (cEventCRID *crid = EventCRIDs.First(); crid; crid = EventCRIDs.Next(crid)) {
      if (!strcmp(suggest->iCRID(), crid->iCRID())) {
	found = true;
	break;
      }
    }
    if (!found) {
      Del(suggest);
      maxNumber--;
      i++;
    }
    suggest = next;
  }
  dsyslog("vdrtva: %d expired suggestions removed", i);
}


/*
	cLinkItem - Entry from the links file
*/

cLinkItem::cLinkItem(const char *sCRID, time_t ModTime, const char *iCRIDs, const char *Path, const char *Title, const char *channelName)
{
  sCrid = strcpyrealloc(NULL, sCRID);
  modtime = ModTime;
  iCrids = strcpyrealloc(NULL, iCRIDs);
  path = strcpyrealloc(NULL, Path);
  title = strcpyrealloc(NULL, Title);
  channelname = strcpyrealloc(NULL, channelName);
}

cLinkItem::~cLinkItem(void)
{
  free(sCrid);
  free(iCrids);
  free(path);
  free(title);
  free(channelname);
}

void cLinkItem::SetModtime(time_t ModTime)
{
  modtime = ModTime;
  Links.SetUpdated();
}

void cLinkItem::SetIcrids(const char *icrids)
{
  iCrids = strcpyrealloc(iCrids, icrids);
  Links.SetUpdated();
}

/*
	cLinks - list of cLinkItem entities
*/

cLinks::cLinks(void)
{
  maxNumber = 0;
  dirty = false;
}

cLinkItem *cLinks::NewLinkItem(const char *sCRID, time_t ModTime, const char *iCRIDs, const char *path, const char *title, const char *channelname)
{
  cLinkItem *NewLinkItem = new cLinkItem(sCRID, ModTime, iCRIDs, path, title, channelname);
  Add(NewLinkItem);
  maxNumber++;
  dirty = true;
  return NewLinkItem;
}

void cLinks::Load()
{
  cString curlinks = AddDirectory(configDir, "links.data");
  FILE *f = fopen(curlinks, "r");
  if (f) {
    char *s;
    char *strtok_next;
    cReadLine ReadLine;
    time_t modtime;
    while ((s = ReadLine.Read(f)) != NULL) {
      char *scrid = strtok_r(s, ";,", &strtok_next);
      char *mtime = strtok_r(NULL, ";", &strtok_next);
      char *icrids = strtok_r(NULL, ";", &strtok_next);
      char *path = strtok_r(NULL, ";", &strtok_next);
      char *title = strtok_r(NULL, ";", &strtok_next);
      char *channelname = strtok_r(NULL, "`", &strtok_next);
      modtime = atoi(mtime);
      if ((path != NULL) && (!strcmp(path, "(NULL)"))) path = NULL;
      NewLinkItem(scrid, modtime, icrids, path, title, channelname);
    }
    fclose (f);
    isyslog("vdrtva: loaded %d series links", MaxNumber());
  }
  else {
    if (f = fopen(curlinks, "w")) {
      isyslog("vdrtva: created new empty series links file");
      fclose (f);
    }
    else esyslog("vdrtva: failed to create new empty series links file");
  }
  dirty = false;
}
  
void cLinks::Save()
{
  if (!dirty) return;
  cString curlinks = AddDirectory(configDir, "links.data");
  cString newlinks = AddDirectory(configDir, "links.new");
  cString oldlinks = AddDirectory(configDir, "links.old");
  FILE *f = fopen(newlinks, "w");
  if (f) {
    for (cLinkItem *Item = First(); Item; Item = Next(Item)) {
      fprintf(f, "%s;%ld;%s", Item->sCRID(), Item->ModTime(), Item->iCRIDs());
      if (Item->Path()) {
	fprintf(f, ";%s", Item->Path());
      }
      else fprintf(f, ";(NULL)");
      if (Item->Title()) {
	fprintf(f, ";%s", Item->Title());
      }
      else fprintf(f, ";(NULL)");
      if (Item->channelName()) {
	fprintf(f, ";%s\n", Item->channelName());
      }
      else fprintf(f, "\n");
    }
    fclose(f);
    unlink (oldlinks);		// Allow to fail if the save file does not exist
    rename (curlinks, oldlinks);
    rename (newlinks, curlinks);
    dirty = false;
    isyslog("vdrtva: saved series links file");
  }
}

// add a new event to the Links table, either as an addition to an existing series or as a new series.
// return false = nothing done, true = new event for old series, or new series.

bool cLinks::AddSeriesLink(const char *scrid, time_t modtime, const char *icrid, const char *path, const char *title, const char *channelName)
{
  if (maxNumber >= 1) {
    cLinkItem * Item = getLinkItem(scrid);
    if (Item != NULL) {
      if (strstr(Item->iCRIDs(), icrid) == NULL) {
	cString icrids = cString::sprintf("%s:%s", Item->iCRIDs(), icrid);
	modtime = max(Item->ModTime(), modtime);
	Item->SetModtime(modtime);
	Item->SetIcrids(icrids);
	isyslog("vdrtva: Adding new event %s to series %s", icrid, scrid);
	return true;
      }
      return false;
    }
  }
  NewLinkItem(scrid, modtime, icrid, path, title, channelName);
  isyslog("vdrtva: Creating new series %s for event %s (%s)", scrid, icrid, title);
  return true;
}


bool cLinks::DeleteItem(const char *sCRID)
{
  if (maxNumber == 0) return false;
  cLinkItem *Item = First();
  while (Item) {
    cLinkItem *next = Next(Item);
    if (!strcmp(Item->sCRID(), sCRID)) {
      DeleteTimersForSCRID(sCRID);
      Del(Item);
      maxNumber--;
      dirty = true;
      return true;
    }
    Item = next;
  }
  return false;
}

void cLinks::DeleteTimersForSCRID(const char *sCRID)
{
  if ((Timers.Count() == 0) || (!captureComplete)) return;
  cTimer *ti = Timers.First();
  while (ti) {
    cTimer *next = Timers.Next(ti);
    const cEvent *event = ti->Event();
    if (event && ti->HasFlags(tfActive) && (ti->WeekDays() == 0)) {
      cChannel *channel = Channels.GetByChannelID(event->ChannelID());
      cChanDA *chanda = ChanDAs.GetByChannelID(channel->Number());
      cEventCRID *eventcrid = EventCRIDs.GetByID(channel->Number(), event->EventID());
      if (eventcrid && chanda) {
	cString scrid = cString::sprintf("%s%s", chanda->DA(),eventcrid->sCRID());
	if (!strcmp(scrid, sCRID)) {
          if (!ti->Recording()) {
	    isyslog ("vdrtva: deleting timer '%s' from deleted series %s", ti->File(), sCRID);
	    Timers.Del(ti);
	    Timers.SetModified();
	  }
	  else esyslog("vdrtva: cannot delete timer '%s': timer is recording", ti->File());
	}
      }
    }
    ti = next;
  }
}

void cLinks::Expire(void)
{
  if (maxNumber == 0) return;
  cLinkItem *Item = First();
  while (Item) {
    cLinkItem *next = Next(Item);
    if ((Item->ModTime() + seriesLifetime) < time(NULL)) {
      isyslog ("vdrtva: Expiring series %s", Item->sCRID());
      Del(Item);
      maxNumber--;
      dirty = true;
    }
    Item = next;
  }
}

cLinkItem * cLinks::getLinkItem(const char *sCRID)
{
  for (cLinkItem *Item = First(); Item; Item = Next(Item)) {
    if (strcasecmp(Item->sCRID(), sCRID) == 0) return Item;
  }
  return NULL;
}

bool cLinks::isEventNeeded(const char *iCRID)
{
  for (cLinkItem *Item = First(); Item; Item = Next(Item)) {
    if (strstr(Item->iCRIDs(), iCRID) != NULL) return false;
  }
  return true;
}

void cLinks::SetUpdated(void)
{
  dirty = true;
}



/*
	cMenuLinkItem - Series Link OSD menu item
*/

cMenuLinkItem::cMenuLinkItem(cLinkItem *LinkItem)
{
  linkitem = LinkItem;
  Set();
}


void cMenuLinkItem::Set(void)
{
  cString buffer;
  char tim[32];
  struct tm tm_r;
  time_t t = linkitem->ModTime();
  tm *tm = localtime_r(&t, &tm_r);
  strftime(tim, sizeof(tim), "%d.%m", tm);
  if (linkitem->Title()) {
    buffer = cString::sprintf("%s\t%s", tim, linkitem->Title());
  }
  else {
    buffer = cString::sprintf("%s\t(No Title)", tim);
  }
  SetText(buffer);
}

int cMenuLinkItem::Compare(const cListObject &ListObject) const
{
  cMenuLinkItem *p = (cMenuLinkItem *)&ListObject;
  return linkitem->ModTime() - p->linkitem->ModTime();
}

//	How many active timers are there for this series?

int cMenuLinkItem::TimerCount(void) {
  int count = 0;
  if ((Timers.Count() == 0) || (!captureComplete)) return 99;
  for (cTimer *ti = Timers.First(); ti; ti = Timers.Next(ti)) {
    const cEvent *event = ti->Event();
    if (event && ti->HasFlags(tfActive) && (ti->WeekDays() == 0)) {
      cChannel *channel = Channels.GetByChannelID(event->ChannelID());
      cChanDA *chanda = ChanDAs.GetByChannelID(channel->Number());
      cEventCRID *eventcrid = EventCRIDs.GetByID(channel->Number(), event->EventID());
      if (eventcrid && chanda) {
	cString scrid = cString::sprintf("%s%s", chanda->DA(),eventcrid->sCRID());
	if (!strcmp(scrid, sCRID())) count++;
      }
    }
  }
  return count;
}


/*
	cMenuLinks - Series Link OSD menu
*/

cMenuLinks::cMenuLinks(void):cOsdMenu(tr("Series Links"), 6)
{
  Clear();
  for (cLinkItem *LinkItem = Links.First(); LinkItem; LinkItem = Links.Next(LinkItem)) {
    cMenuLinkItem *item = new cMenuLinkItem(LinkItem);
    Add(item);
  }
  Sort();
  SetHelp(tr("Delete"), tr("Info"));
  Display();
}

void cMenuLinks::Propagate(void)
{
  for (cMenuLinkItem *ci = (cMenuLinkItem *)First(); ci; ci = (cMenuLinkItem *)ci->Next())
      ci->Set();
  Display();
}

eOSState cMenuLinks::ProcessKey(eKeys Key)
{
  eOSState state = cOsdMenu::ProcessKey(Key);

  if (state == osUnknown) {
     switch (Key) {
       case kRed:	return Delete();
       case kGreen:	return Info();
       case kYellow:
       case kBlue:
       case kOk:
       default:      state = osContinue;
       }
     }
  return state;
}

eOSState cMenuLinks::Delete(void)
{
  if (HasSubMenu() || Count() == 0) return osContinue;
  if (!captureComplete) {
    Skins.Message(mtError, tr("Data capture still in progress"));
    return osContinue;
  }
  int Index = Current();
  cMenuLinkItem *item = (cMenuLinkItem *)Get(Index);
  int timercount = item->TimerCount();
  cString prompt;
  if (timercount > 1) {
    prompt = cString::sprintf(tr("Delete series link & %d timers?"), timercount);
  }
  else if (timercount == 1) {
    prompt = cString::sprintf(tr("Delete series link & 1 timer?"));
  }
  else {
    prompt = cString::sprintf(tr("Delete series link?"));
  }
  if (Interface->Confirm(prompt)) {
    char *linkCRID = item->sCRID();
    cOsdMenu::Del(Index);
    Propagate();
    isyslog("vdrtva: series link %s deleted by OSD", linkCRID);
    Links.DeleteItem(linkCRID);
  }
  return osContinue;
}

eOSState cMenuLinks::Info(void)
{
  if (HasSubMenu() || Count() == 0) return osContinue;
  if (!captureComplete) {
    Skins.Message(mtError, tr("Data capture still in progress"));
    return osContinue;
  }
  int Index = Current();
  cMenuLinkItem *menuitem = (cMenuLinkItem *)Get(Index);
  cLinkItem *linkitem = menuitem->LinkItem();
  char *icrids = linkitem->iCRIDs();
  int eventcount = 1;
  while (icrids = strchr(icrids, ':')) {
    eventcount++;
    icrids++;
  }
  cString message = cString::sprintf("Series CRID:      %s\nTotal Events:    %d\nActive Timers:   %d", 
				menuitem->sCRID(), eventcount, menuitem->TimerCount());
  if (linkitem->Title()) {
    return AddSubMenu(new cMenuText(linkitem->Title(), message, fontOsd));
  }
  else {
    return AddSubMenu(new cMenuText(tr("(No Title)"), message, fontOsd));
  }
}


#if VDRVERSNUM < 10728

// --- cSortedTimers (copied from timers.c v1.7.29) ---

static int CompareTimers(const void *a, const void *b)
{
  return (*(const cTimer **)a)->Compare(**(const cTimer **)b);
}

cSortedTimers::cSortedTimers(void)
:cVector<const cTimer *>(Timers.Count())
{
  for (const cTimer *Timer = Timers.First(); Timer; Timer = Timers.Next(Timer))
      Append(Timer);
  Sort(CompareTimers);
}
#endif

VDRPLUGINCREATOR(cPluginvdrTva); // Don't touch this!
