#ifndef _BDM_SUPPORTCLASSES_H_
#define _BDM_SUPPORTCLASSES_H_

#include <vector>
#include <atomic>
#include <functional>

#include "BinaryData.h"
#include "ScrAddrObj.h"
#include "BtcWallet.h"


class ZeroConfContainer;

struct ZeroConfData
{
   Tx            txobj_;
   uint32_t      txtime_;

   bool operator==(const ZeroConfData& rhs) const
   {
      return (this->txobj_ == rhs.txobj_) && (this->txtime_ == rhs.txtime_);
   }
};

class ScrAddrFilter
{
   /***
   This class keeps track of all registered scrAddr to be scanned by the DB.
   If the DB isn't running in supernode, this class also acts as a helper to
   filter transactions, which is required in order to save only relevant SSH

   The transaction filter isn't exact however. It gets more efficient as it
   encounters more UTxO.

   The basic principle of the filter is that it expect to have a complete
   list of UTxO's starting a given height, usually where the DB picked up
   at initial load. It can then guarantee a TxIn isn't spending a tracked
   UTxO by checking the UTxO DBkey instead of fetching the entire stored TxOut.
   If the DBkey carries a height lower than the cut off, the filter will
   fail to give a definitive answer, in which case the TxOut script will be
   pulled from the DB, using the DBkey, as it would have otherwise.

   Registering addresses while the BDM isn't initialized will return instantly
   Otherwise, the following steps are taken:

   1) Check SSH entries in the DB for this scrAddr. If there is none, this
   DB never saw this address (full/lite node). Else mark the top scanned block.

   -- Non supernode operations --
   2.a) If the address is new, create an empty SSH header for that scrAddr
   in the DB, marked at the current top height
   2.b) If the address isn't new, scan it from its last seen block, or its
   block creation, or 0 if none of the above is available. This will create
   the SSH entries for the address, which will have the current top height as
   its scanned height.
   --

   3) Add address to scrAddrMap_

   4) Signal the wallet that the address is ready. Wallet object will take it
   up from there.
   ***/

   friend class BlockDataViewer;

public:
   struct ScrAddrMeta
   {
      /***
      scrAddrMap_ is a map so it can only have meta per scrAddr. This means
      only 1 wallet can be registered per post BDM init address scan.
      ***/
      uint32_t lastScannedHeight_=0;
      BtcWallet* wltPtr_=nullptr;

      ScrAddrMeta(uint32_t height=0, BtcWallet* wltPtr = nullptr) :
         lastScannedHeight_(height),
         wltPtr_(wltPtr) {}
   };

private:
   //map of scrAddr and their respective last scanned block
   //this is used only for the inital load currently
   map<BinaryData, ScrAddrMeta>   scrAddrMap_;

   set<BinaryData>                UTxO_;
   mutable uint32_t               blockHeightCutOff_=0;
   LMDBBlockDatabase *const       lmdb_;

   //
   ScrAddrFilter*                 parent_;
   set<BinaryData>                UTxOToMerge_;
   map<BinaryData, ScrAddrMeta>   scrAddrMapToMerge_;
   atomic<int32_t>                mergeLock_;
   bool                           mergeFlag_=false;
   bool                           freshAddresses_=false;

   void setScrAddrLastScanned(const BinaryData& scrAddr, uint32_t blkHgt)
   {
      map<BinaryData, ScrAddrMeta>::iterator scrAddrIter =
         scrAddrMap_.find(scrAddr);
      if (ITER_IN_MAP(scrAddrIter, scrAddrMap_))
      {
         scrAddrIter->second.lastScannedHeight_ = blkHgt;
         blockHeightCutOff_ = max(blockHeightCutOff_, blkHgt);
      }
   }

public:
   
   const ARMORY_DB_TYPE           armoryDbType_;
  
   ScrAddrFilter(LMDBBlockDatabase* lmdb, ARMORY_DB_TYPE armoryDbType)
      : lmdb_(lmdb), mergeLock_(0), armoryDbType_(armoryDbType)
   {}
   
   virtual ~ScrAddrFilter() { }
   
   LMDBBlockDatabase* lmdb() { return lmdb_; }

   const map<BinaryData, ScrAddrMeta>& getScrAddrMap(void) const
   { return scrAddrMap_; }

   size_t numScrAddr(void) const
   { return scrAddrMap_.size(); }

   uint32_t scanFrom(void) const;
   bool registerAddresses(const vector<BinaryData>& saVec, 
                          BtcWallet* wltPtr, bool isNew);

   void unregisterScrAddr(BinaryData& scrAddrIn)
   { scrAddrMap_.erase(scrAddrIn); }

   void clear(void);

   bool hasScrAddress(const BinaryData & sa)
   { return (scrAddrMap_.find(sa) != scrAddrMap_.end()); }

   int8_t hasUTxO(const BinaryData& dbkey) const;

   void addUTxO(pair<const BinaryData, TxIOPair>& txio);
   void addUTxO(const BinaryData& dbkey);

   bool eraseUTxO(const BinaryData& dbkey)
   { return UTxO_.erase(dbkey) == 1; }

   void getScrAddrCurrentSyncState();
   void getScrAddrCurrentSyncState(BinaryData const & scrAddr);

   void setSSHLastScanned(uint32_t height);

   void regScrAddrForScan(const BinaryData& scrAddr, uint32_t scanFrom,
      BtcWallet* wltPtr)
   { scrAddrMap_[scrAddr] = ScrAddrMeta(scanFrom, wltPtr); }

   void scanScrAddrMapInNewThread(void);

   void setParent(ScrAddrFilter* sca) { parent_ = sca; }
   void merge(void);
   void checkForMerge(void);

protected:
   virtual bool bdmIsRunning() const=0;
   virtual void applyBlockRangeToDB(
      uint32_t startBlock, uint32_t endBlock, BtcWallet *wltPtr
   )=0;
   virtual uint32_t currentTopBlockHeight() const=0;
   virtual ScrAddrFilter *copy()=0;
   
private:
   static void* scanScrAddrThread(void *in);
};

class ZeroConfContainer
{
   /***
   This class does parses ZC based on a filter function that takes a scrAddr
   and return a bool. 

   This class stores and represents ZC transactions by DBkey. While the ZC txn
   do not hit the DB, they are assigned a 6 bytes key like mined transaction
   to unify TxIn parsing by DBkey.

   DBkeys are unique. They are preferable to outPoints because they're cheaper
   (8 bytes vs 32), and do not incur extra processing to recover a TxOut
   script DB (TxOuts are saved by DBkey, but OutPoints only store a TxHash, 
   which has to be converted first to a DBkey). They also carry height,
   dupID and TxId natively.

   The first 2 bytes of ZC DBkey will always be 0xFFFF. The
   transaction index having to be unique, will be 4 bytes long instead, and
   produced by atomically incrementing topId_. In comparison, a TxOut DBkey
   uses 4 Bytes for height and dupID, 2 bytes for the Tx index in the block it 
   refers to by hgtX, and 2 more bytes for the TxOut id. 

   Indeed, at 7 tx/s, including limbo, it is possible a 2 bytes index will
   overflow on long run cycles.

   Methods:
   addRawTx takes in a raw tx, hashes it and verifies it isnt already added.
   It then unserializes the transaction to a Tx Object, assigns it a key and
   parses it to populate the TxIO map. It returns the Tx key if valid, or an
   empty BinaryData object otherwise.
   ***/

private:
   map<HashString, HashString>                  txHashToDBKey_;
   map<HashString, Tx>                          txMap_;
   map<HashString, map<BinaryData, TxIOPair> >  txioMap_;

   std::atomic<uint32_t>       topId_;

   atomic<uint32_t>            lock_;

   //newZCmap_ is ephemeral. Raw ZC are saved until they are processed.
   //The code has a thread pushing new ZC, and set the BDM thread flag
   //to parse it
   map<BinaryData, Tx> newZCMap_;

   //newTxioMap_ is ephemeral too. It's contains ZC txios that have yet to be
   //processed by their relevant scrAddrObj. It's content is returned then wiped 
   //by each call to getNewTxioMap
   map<HashString, map<BinaryData, TxIOPair> >  newTxioMap_;
   LMDBBlockDatabase*                           db_;


   BinaryData getNewZCkey(void);
   bool RemoveTxByKey(const BinaryData key);
   bool RemoveTxByHash(const BinaryData txHash);
   map<BinaryData, map<BinaryData, TxIOPair> >
      ZCisMineBulkFilter(const Tx & tx,
      const BinaryData& ZCkey,
      uint32_t txtime,
      function<bool(const BinaryData&)>,
      bool withSecondOrderMultisig = true) const;

public:
   ZeroConfContainer(LMDBBlockDatabase* db) :
      topId_(0), lock_(0), db_(db) {}

   void addRawTx(const BinaryData& rawTx, uint32_t txtime);

   bool hasTxByHash(const BinaryData& txHash) const;
   Tx getTxByHash(const BinaryData& txHash) const;

   map<BinaryData, vector<BinaryData> > purge(
      function<bool(const BinaryData&)>);

   const map<HashString, map<BinaryData, TxIOPair> >& 
      getNewTxioMap(void);
   const map<HashString, map<BinaryData, TxIOPair> >&
      getFullTxioMap(void) const { return txioMap_; }

   //returns a vector of ZC TxHash that belong to your tracked scrAddr. This is
   //mostly a UI helper method
   set<BinaryData> getNewZCByHash(void) const;

   bool parseNewZC(function<bool(const BinaryData&)>);

   bool getKeyForTxHash(const BinaryData& txHash, BinaryData zcKey) const;

   void resetNewZC() { newTxioMap_.clear(); }

   void clear(void);
};

#endif
// kate: indent-width 3; replace-tabs on;