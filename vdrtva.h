#include <vdr/filter.h>
#include <vdr/device.h>
#include <vdr/status.h>
#include <vdr/interface.h>
#include <vdr/menu.h>


class cTvaFilter : public cFilter {
private:
  cSectionSyncer sectionSyncer;
  cPatFilter *patFilter;
protected:
  virtual void Process(u_short Pid, u_char Tid, const u_char *Data, int Length);
public:
  cTvaFilter(void);
}; 

class cTvaStatusMonitor : public cStatus {
  private:
    time_t timeradded;
    const cTimer *lasttimer;
  protected:
    virtual void TimerChange(const cTimer *Timer, eTimerChange Change);
               // Indicates a change in the timer settings.
               // If Change is tcAdd or tcDel, Timer points to the timer that has
               // been added or will be deleted, respectively. In case of tcMod,
               // Timer is NULL; this indicates that some timer has been changed.
               // Note that tcAdd and tcDel are always also followed by a tcMod.
  public:
    cTvaStatusMonitor(void);
    int GetTimerAddedDelta(void);
    void ClearTimerAdded(void);
    const cTimer *GetLastTimer() { return lasttimer; }
};


class cTvaMenuSetup : public cMenuSetupPage {
private:
  int newcollectionperiod;
  int newlifetime;
  int newpriority;
  int newseriesLifetime;
  int newupdatetime;
protected:
  virtual void Store(void);
public:
  cTvaMenuSetup(void);
};


class cTvaLog {
  private:
    char *buffer, *mailfrom, *mailto;
    int length;
    int size;
  public:
    cTvaLog(void);
    ~cTvaLog(void);
    void Append(const char *Fmt, ...);
    const char* Buffer() { return buffer; }
    int Length(void);
    void Clear() { length = 0; }
    void MailLog(void);
    void setmailTo(char *opt);
    const char * mailTo() { return mailto; }
    void setmailFrom(char *opt);
    const char * mailFrom() { return mailfrom; }
};


class cChanDA : public cListObject {
  private:
    int cid;
    char *defaultAuthority;
  public:
    cChanDA(int Cid, char *DA);
    ~cChanDA(void);
    int Cid(void) { return cid; }
    char * DA(void) { return defaultAuthority; }
};

class cChanDAs : public cRwLock, public cConfig<cChanDA> {
  private:
    int maxNumber;
    cHash<cChanDA> chanDAHash;
  public:
    cChanDAs(void);
    ~cChanDAs(void);
    int MaxNumber(void) { return maxNumber; }
    cChanDA *GetByChannelID(int cid);
    cChanDA *NewChanDA(int Cid, char *DA);
};


class cEventCRID : public cListObject {
  private:
    tEventID eid;
    int cid;
    char *iCrid;
    char *sCrid;
  public:
    cEventCRID(int Cid, tEventID Eid, char *iCRID, char *sCRID);
    ~cEventCRID(void);
    tEventID Eid(void) { return eid; }
    char * iCRID(void) { return iCrid; }
    char * sCRID(void) { return sCrid; }
    int Cid(void) { return cid; }
};

class cEventCRIDs : public cRwLock, public cConfig<cEventCRID> {
  private:
    int maxNumber;
    cHash<cEventCRID> EventCRIDHash;
  public:
    cEventCRIDs(void);
    ~cEventCRIDs(void);
    int MaxNumber(void) { return maxNumber; }
    cEventCRID *GetByID(int Cid, tEventID Eid);
    cEventCRID *NewEventCRID(int Cid, tEventID Eid, char *iCRID, char *sCRID);
    void Expire(void);
};


class cSuggestCRID : public cListObject {
  private:
    char *iCrid;
    char *gCrid;
    int cid;
  public:
    cSuggestCRID(int Cid, char *iCRID, char *gCRID);
    ~cSuggestCRID(void);
    char * iCRID(void) { return iCrid; }
    char * gCRID(void) { return gCrid; }
    int Cid(void) { return cid; }
    virtual int Compare(const cListObject &ListObject) const;
};


class cSuggestCRIDs : public cRwLock, public cConfig<cSuggestCRID> {
  private:
    int maxNumber;
  public:
    cSuggestCRIDs(void);
    int MaxNumber(void) { return maxNumber; }
    cSuggestCRID *NewSuggestCRID(int Cid, char *icrid, char *gcrid);
    void DeDup(void);
    void Expire(void);
};


class cLinkItem : public cListObject {
  private:
    char *sCrid;
    time_t modtime;
    char *iCrids;
    char *path;
    char *title;
  public:
    cLinkItem(const char *sCRID, time_t ModTime, const char *iCRIDs, const char *Path, const char *Title);
    ~cLinkItem(void);
    void SetModtime(time_t modtime);
    void SetIcrids(const char *icrids);
    char * iCRIDs(void) { return iCrids; }
    char * sCRID(void) { return sCrid; }
    time_t ModTime(void) { return modtime; }
    char * Path(void) { return path; }
    char * Title(void) { return title; }
};

class cLinks : public cRwLock, public cConfig<cLinkItem> {
  private:
    int maxNumber;
    bool dirty;
  public:
    cLinks(void);
    int MaxNumber(void) { return maxNumber; }
    cLinkItem *NewLinkItem(const char *sCRID, time_t ModTime, const char *iCRIDs, const char *Path, const char *Title);
    void Load(void);
    void Save(void);
    bool DeleteItem(const char *sCRID);
    void Expire(void);
    void SetUpdated(void);
    void DeleteTimersForSCRID(const char *sCRID);
};


class cMenuLinks : public cOsdMenu {
private:
  void Propagate(void);
  eOSState Delete(void);
  eOSState Info(void);
public:
  cMenuLinks(void);
  virtual eOSState ProcessKey(eKeys Key);
};

class cMenuLinkItem : public cOsdItem {
private:
  cLinkItem *linkitem;
public:
  cMenuLinkItem(cLinkItem *LinkItem);
  char * sCRID(void) { return linkitem->sCRID(); }
  cLinkItem * LinkItem(void) { return linkitem; }
  int TimerCount(void);
  virtual void Set(void);
  virtual int Compare(const cListObject &ListObject) const;
};

#if VDRVERSNUM < 10728

// Copied from timers.c v1.7.29

class cSortedTimers : public cVector<const cTimer *> {
public:
  cSortedTimers(void);
  };
#endif
