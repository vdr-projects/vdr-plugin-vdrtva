#include <vdr/filter.h>
#include <vdr/device.h>

class cTvaFilter : public cFilter {
private:
  cSectionSyncer sectionSyncer;
  cPatFilter *patFilter;
protected:
  virtual void Process(u_short Pid, u_char Tid, const u_char *Data, int Length);
public:
  cTvaFilter(void);
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
