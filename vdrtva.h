#include <vdr/filter.h>
#include <vdr/device.h>
#include <vdr/status.h>

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
    cChanDA(void);
    ~cChanDA(void);
    int Cid(void) { return cid; }
    void Set(int Cid);
    char * DA(void) { return defaultAuthority; }
    void SetDA(char *DA);
};

class cChanDAs : public cRwLock, public cConfig<cChanDA> {
  private:
    int maxNumber;
    cHash<cChanDA> chanDAHash;
  public:
    cChanDAs(void);
    ~cChanDAs(void);
    int MaxNumber(void) { return maxNumber; }
    void SetMaxNumber(int number) { maxNumber = number; }
    cChanDA *GetByChannelID(int cid);
    cChanDA *NewChanDA(int Cid);
};


class cEventCRID : public cListObject {
  private:
    tEventID eid;
    int cid;
    char *iCrid;
    char *sCrid;
  public:
    cEventCRID(void);
    ~cEventCRID(void);
    tEventID Eid(void) { return eid; }
    void Set(int Cid, tEventID Eid);
    char * iCRID(void) { return iCrid; }
    char * sCRID(void) { return sCrid; }
    void SetCRIDs(char *iCRID, char *sCRID);
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
    void SetMaxNumber(int number) { maxNumber = number; }
    cEventCRID *GetByID(int Cid, tEventID Eid);
    cEventCRID *NewEventCRID(int Cid, tEventID Eid);
};


class cSuggestCRID : public cListObject {
  private:
    char *iCrid;
    char *gCrid;
    int cid;
  public:
    cSuggestCRID(void);
    ~cSuggestCRID(void);
    char * iCRID(void) { return iCrid; }
    char * gCRID(void) { return gCrid; }
    int Cid(void) { return cid; }
    void Set(int Cid, char *iCRID, char *gCRID);
    virtual int Compare(const cListObject &ListObject) const;
};


class cSuggestCRIDs : public cRwLock, public cConfig<cSuggestCRID> {
  private:
    int maxNumber;
  public:
    cSuggestCRIDs(void);
    ~cSuggestCRIDs(void);
    int MaxNumber(void) { return maxNumber; }
    void SetMaxNumber(int number) { maxNumber = number; }
    cSuggestCRID *NewSuggestCRID(int Cid, char *icrid, char *gcrid);
};


class cLinkItem : public cListObject {
  private:
    char *sCrid;
    int modtime;
    char *iCrids;
  public:
    cLinkItem(void);
    ~cLinkItem(void);
    void Set(const char *sCRID, int ModTime, const char *iCRIDs);
    char * iCRIDs(void) { return iCrids; }
    char * sCRID(void) { return sCrid; }
    int ModTime(void) { return modtime; }
};

class cLinks : public cRwLock, public cConfig<cLinkItem> {
  private:
    int maxNumber;
  public:
    cLinks(void);
//    ~cLinks(void);
    int MaxNumber(void) { return maxNumber; }
    void SetMaxNumber(int number) { maxNumber = number; }
    cLinkItem *NewLinkItem(const char *sCRID, int ModTime, const char *iCRIDs);
};
