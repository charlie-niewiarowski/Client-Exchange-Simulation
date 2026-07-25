//
// Created by cniew on 7/25/26.
//

/*
Alpha fields may contain upper and lowercase characters.

All fixed-width alpha fields are left-justified and padded on the right with spaces.

All Numeric fields are binary formatted, little-endian numbers. Four flavors of numeric fields are supported: Longs (8 bytes),
Integers (4 bytes), Shorts (2 bytes), and Bytes (1 byte).

Sizes (shares quantities, field lengths) should be treated as unsigned values.

Prices are numeric fields with an implied 4 decimal places. Prices are to be treated as unsigned numeric fields, unless
designated otherwise. The maximum price currently supported is $199,999.9900 (decimal, 7735939C hex). To flag an order
as a market order for a cross, use the special price of $214,748.3647 (decimal, 7FFFFFFF hex). Orders entered with a price of
$200,000.00 or the max integer value will also be treated as market orders.

A UserRefNum is an unsigned numeric. For a given OUCH port, the UserRefNum is used as a transaction identifier, and must
be both unique and strictly increasing throughout the trading day. The UserRefNum begins at 1 and the system ignores new
order requests identified with UserRefNums lower than the last one processed, assuming they are retransmissions.

Client however has an option to multiplex their order flow via the use of an optional tag UserRefIdx. When this tag is present
the UserRefNum is then assumed to be linked to an “order flow channel” specified by the UserRefIdx and the above rules
regarding the use of UserRefNum are applied only within this “order flow channel” instead of the port globally.

ClOrdID is alphanumeric and non-binary. All letters and numbers are allowed, as well as spaces.

An optional attribute on an order is communicated via a TagValue element.
*/

#ifndef OUCH_HPP
#define OUCH_HPP

#include <array>
#include <cstdint>

namespace ouch {

//=============================================================================
// Numeric primitives
//=============================================================================

using Long    = uint64_t;
using Integer = uint32_t;
using Short   = uint16_t;
using Byte    = uint8_t;

//=============================================================================
// Semantic scalar fields
//=============================================================================

using Timestamp            = uint64_t; // nanoseconds since midnight
using UserRefNum           = uint32_t; // day-unique, strictly increasing
using Quantity             = uint32_t; // shares; 0 < qty < 1,000,000
using Price                = uint32_t; // unsigned, implied 4 decimals
using SignedPrice          = int32_t;  // signed price (e.g. PegOffset)
using OrderReferenceNumber = uint64_t; // day-unique, assigned by NASDAQ
using MatchNumber          = uint64_t; // trade identifier
using AppendageLength      = uint16_t; // length of the optional appendage
using RejectReason         = uint16_t; // numeric reason (Appendix C)
using UserRefIndex         = uint8_t;  // order-flow channel (UserRefIdx tag)

//=============================================================================
// Fixed-width alpha fields (left-justified, space-padded)
//=============================================================================

using Symbol  = std::array<char, 8>;  // stock symbol
using ClOrdID = std::array<char, 14>; // customer order identifier (alphanumeric)
using Firm    = std::array<char, 4>;  // firm identifier, all caps

//=============================================================================
// Message type codes
//=============================================================================

enum class InboundType : char { // participant -> host
    EnterOrder   = 'O',
    ReplaceOrder = 'U', // cancel/replace: new price, new time priority
    CancelOrder  = 'X',
    ModifyOrder  = 'M', // priority-preserving: side flip / quantity reduction only
    MassCancel   = 'C',
    AccountQuery = 'Q', // request the next available UserRefNum (state recovery)
};

enum class OutboundType : char { // host -> participant
    SystemEvent        = 'S',
    Accepted           = 'A', // ack of Enter Order
    Replaced           = 'U', // ack of Replace Order
    Canceled           = 'C', // order reduced / canceled
    Executed           = 'E', // fill
    BrokenTrade        = 'B',
    Rejected           = 'J', // Enter / Replace rejected
    CancelPending      = 'P',
    CancelReject       = 'I',
    PriorityUpdate     = 'T',
    Modified           = 'M', // ack of Modify Order
    Restated           = 'R',
    MassCancelResponse = 'X',
    AccountQueryResp   = 'Q',
};

//=============================================================================
// Coded single-character (Alpha) fields, shared by inbound and outbound
//=============================================================================

enum class Side : char {
    Buy             = 'B',
    Sell            = 'S',
    SellShort       = 'T',
    SellShortExempt = 'E',
};

enum class TimeInForce : char {
    Day        = '0', // market hours
    IOC        = '3',
    GTX        = '5', // extended hours
    GTT        = '6', // ExpireTime required
    AfterHours = 'E',
};

enum class Display : char {
    Visible      = 'Y',
    Hidden       = 'N',
    Attributable = 'A',
    Conformant   = 'Z', // outbound-only (Accepted / Replaced)
};

enum class Capacity : char {
    Agency    = 'A',
    Principal = 'P',
    Riskless  = 'R',
    Other     = 'O',
};

enum class IntermarketSweepEligibility : char {
    Eligible    = 'Y',
    NotEligible = 'N',
};

enum class CrossType : char {
    ContinuousMarket = 'N',
    OpeningCross     = 'O',
    ClosingCross     = 'C',
    HaltIPO          = 'H',
    Supplemental     = 'S',
    Retail           = 'R', // BX only
    ExtendedLife     = 'E',
    AfterHoursClose  = 'A',
};

enum class OrderState : char {
    Live = 'L',
    Dead = 'D',
};

//=============================================================================
// Special price sentinels (see Data Types)
//=============================================================================

inline constexpr Price PRICE_MARKET_CROSS  = 0x7FFFFFFFULL;    // $214,748.3647
inline constexpr Price PRICE_MARKET_200K   = 2'000'000'000ULL; // $200,000.0000
inline constexpr Price PRICE_MAX_SUPPORTED = 1'999'999'900ULL; // $199,999.9900

//=============================================================================
// Inbound messages (participant -> host)
//=============================================================================
//
// The structs are packed wire images (alignment 1, native little-endian). The
// optional TagValue appendage is out of scope; every message carries its
// AppendageLength field, which we always emit as 0 (no appendage). Offsets in
// the comments are byte offsets into this packed layout.

namespace inbound {

#pragma pack(push, 1)

// Type O - Enter Order
struct EnterOrder {
    InboundType type = InboundType::EnterOrder; //  0
    UserRefNum  user_ref_num;                   //  1
    Side        side;                           //  5
    Quantity    quantity;                       //  6
    Symbol      symbol;                         // 10
    Price       price;                          // 18
    TimeInForce time_in_force;                  // 22
    Display     display;                        // 23
    Capacity    capacity;                       // 24
    IntermarketSweepEligibility ise;            // 25
    CrossType   cross_type;                     // 26
    ClOrdID     cl_ord_id;                      // 27
    AppendageLength appendage_length = 0;       // 41
};                                              // 43 bytes

// Type U - Replace Order Request
struct ReplaceOrder {
    InboundType type = InboundType::ReplaceOrder; //  0
    UserRefNum  orig_user_ref_num;                //  1  existing order
    UserRefNum  user_ref_num;                     //  5  replacement order
    Quantity    quantity;                         //  9
    Price       price;                            // 13
    TimeInForce time_in_force;                    // 17
    Display     display;                          // 18
    IntermarketSweepEligibility ise;              // 19
    ClOrdID     cl_ord_id;                        // 20
    AppendageLength appendage_length = 0;         // 34
};                                                // 36 bytes

// Type X - Cancel Order Request
struct CancelOrder {
    InboundType type = InboundType::CancelOrder; //  0
    UserRefNum  user_ref_num;                    //  1
    Quantity    quantity;                        //  5  new intended size; 0 cancels the balance
    AppendageLength appendage_length = 0;        //  9
};                                               // 11 bytes

// Type M - Modify Order Request
struct ModifyOrder {
    InboundType type = InboundType::ModifyOrder; //  0
    UserRefNum  user_ref_num;                    //  1
    Side        side;                            //  5  new side (S->E, S->T, E->T only)
    Quantity    quantity;                        //  6  new intended size (decrease only)
    AppendageLength appendage_length = 0;        // 10
};                                               // 12 bytes

// Type C - Mass Cancel Request
struct MassCancel {
    InboundType type = InboundType::MassCancel;  //  0
    UserRefNum  user_ref_num;                    //  1
    Firm        firm;                            //  5  all caps
    Symbol      symbol;                          //  9  optional, space-filled if unspecified
    AppendageLength appendage_length = 0;        // 17
};                                               // 19 bytes

// Type Q - Account Query Request
struct AccountQuery {
    InboundType type = InboundType::AccountQuery; //  0
    AppendageLength appendage_length = 0;         //  1
};                                                //  3 bytes

#pragma pack(pop)

static_assert(sizeof(EnterOrder)   == 43);
static_assert(sizeof(ReplaceOrder) == 36);
static_assert(sizeof(CancelOrder)  == 11);
static_assert(sizeof(ModifyOrder)  == 12);
static_assert(sizeof(MassCancel)   == 19);
static_assert(sizeof(AccountQuery) ==  3);

} // namespace inbound

//=============================================================================
// Outbound messages (host -> participant)
//=============================================================================

namespace outbound {

enum class SystemEventCode : char {
    StartOfDay = 'S', // exchange open, accepting orders
    EndOfDay   = 'E', // exchange closed
};

// Appendix B - Order Cancel Reasons
enum class CancelReason : char {
    RegulatoryRestriction = 'D',
    Closed                = 'E',
    PostOnlyCancelNMS     = 'F',
    PostOnlyCancelContra  = 'G',
    Halted                = 'H',
    ImmediateOrCancel     = 'I',
    MarketCollars         = 'K',
    SelfMatchPrevention   = 'Q',
    Supervisory           = 'S',
    Timeout               = 'T',
    UserRequested         = 'U',
    OpenProtection        = 'X',
    SystemCancel          = 'Z',
};

// Broken Trade reasons (Type B)
enum class BrokenTradeReason : char {
    Erroneous   = 'E', // deemed clearly erroneous
    Consent     = 'C', // both parties agreed to break
    Supervisory = 'S', // manually broken by supervisory
    External    = 'X', // broken by an external third party
};

// Order Restated reasons (Type R)
enum class RestateReason : char {
    RefreshOfDisplay    = 'R', // refresh of display on an order with reserves
    UpdatedDisplayPrice = 'P', // update of displayed price
};

#pragma pack(push, 1)

// Type S - System Event
struct SystemEvent {
    OutboundType    type = OutboundType::SystemEvent; //  0
    Timestamp       timestamp;                        //  1
    SystemEventCode event_code;                       //  9
};                                                    // 10 bytes

// Type A - Order Accepted
struct Accepted {
    OutboundType type = OutboundType::Accepted; //  0
    Timestamp   timestamp;                       //  1
    UserRefNum  user_ref_num;                    //  9
    Side        side;                            // 13
    Quantity    quantity;                        // 14
    Symbol      symbol;                          // 18
    Price       price;                           // 26
    TimeInForce time_in_force;                   // 30
    Display     display;                         // 31
    OrderReferenceNumber order_ref_num;          // 32
    Capacity    capacity;                        // 40
    IntermarketSweepEligibility ise;             // 41
    CrossType   cross_type;                      // 42
    OrderState  order_state;                     // 43
    ClOrdID     cl_ord_id;                       // 44
    AppendageLength appendage_length = 0;        // 58
};                                               // 60 bytes

// Type U - Order Replaced
struct Replaced {
    OutboundType type = OutboundType::Replaced; //  0
    Timestamp   timestamp;                       //  1
    UserRefNum  orig_user_ref_num;               //  9
    UserRefNum  user_ref_num;                    // 13
    Side        side;                            // 17
    Quantity    quantity;                        // 18
    Symbol      symbol;                          // 22
    Price       price;                           // 30
    TimeInForce time_in_force;                   // 34
    Display     display;                         // 35
    OrderReferenceNumber order_ref_num;          // 36
    Capacity    capacity;                        // 44
    IntermarketSweepEligibility ise;             // 45
    CrossType   cross_type;                      // 46
    OrderState  order_state;                     // 47
    ClOrdID     cl_ord_id;                       // 48
    AppendageLength appendage_length = 0;        // 62
};                                               // 64 bytes

// Type C - Order Canceled
struct Canceled {
    OutboundType type = OutboundType::Canceled; //  0
    Timestamp    timestamp;                      //  1
    UserRefNum   user_ref_num;                   //  9
    Quantity     quantity;                        // 13  shares decremented (incremental)
    CancelReason reason;                          // 17
    AppendageLength appendage_length = 0;         // 18
};                                                // 20 bytes

// Type E - Order Executed
struct Executed {
    OutboundType type = OutboundType::Executed; //  0
    Timestamp   timestamp;                       //  1
    UserRefNum  user_ref_num;                    //  9
    Quantity    quantity;                         // 13  shares executed (incremental)
    Price       price;                            // 17
    char        liquidity_flag;                   // 21  Appendix D liquidity code
    MatchNumber match_number;                     // 22
    AppendageLength appendage_length = 0;         // 30
};                                                // 32 bytes

// Type B - Broken Trade
struct BrokenTrade {
    OutboundType type = OutboundType::BrokenTrade; //  0
    Timestamp    timestamp;                        //  1
    UserRefNum   user_ref_num;                     //  9
    MatchNumber  match_number;                     // 13  match number of the broken execution
    BrokenTradeReason reason;                      // 21
    ClOrdID      cl_ord_id;                        // 22
    AppendageLength appendage_length = 0;          // 36
};                                                 // 38 bytes

// Type J - Rejected
struct Rejected {
    OutboundType type = OutboundType::Rejected; //  0
    Timestamp    timestamp;                      //  1
    UserRefNum   user_ref_num;                   //  9
    RejectReason reason;                          // 13  Appendix C numeric reason
    ClOrdID      cl_ord_id;                       // 15
    AppendageLength appendage_length = 0;         // 29
};                                                // 31 bytes

// Type P - Cancel Pending
struct CancelPending {
    OutboundType type = OutboundType::CancelPending; //  0
    Timestamp   timestamp;                           //  1
    UserRefNum  user_ref_num;                        //  9
    AppendageLength appendage_length = 0;            // 13
};                                                   // 15 bytes

// Type I - Cancel Reject
struct CancelReject {
    OutboundType type = OutboundType::CancelReject; //  0
    Timestamp   timestamp;                          //  1
    UserRefNum  user_ref_num;                       //  9
    AppendageLength appendage_length = 0;           // 13
};                                                  // 15 bytes

// Type T - Order Priority Update
struct PriorityUpdate {
    OutboundType type = OutboundType::PriorityUpdate; //  0
    Timestamp   timestamp;                            //  1
    UserRefNum  user_ref_num;                         //  9
    Price       price;                                // 13  limit price of the order
    Display     display;                              // 17
    OrderReferenceNumber order_ref_num;               // 18  new ref number from re-priority
    AppendageLength appendage_length = 0;             // 26
};                                                    // 28 bytes

// Type M - Order Modified
struct Modified {
    OutboundType type = OutboundType::Modified; //  0
    Timestamp  timestamp;                        //  1
    UserRefNum user_ref_num;                     //  9
    Side       side;                             // 13
    Quantity   quantity;                         // 14  shares outstanding
    AppendageLength appendage_length = 0;        // 18
};                                               // 20 bytes

// Type R - Order Restated
struct Restated {
    OutboundType  type = OutboundType::Restated; //  0
    Timestamp     timestamp;                     //  1
    UserRefNum    user_ref_num;                  //  9
    RestateReason reason;                         // 13
    AppendageLength appendage_length = 0;         // 14
};                                                // 16 bytes

// Type X - Mass Cancel Response
struct MassCancelResponse {
    OutboundType type = OutboundType::MassCancelResponse; //  0
    Timestamp   timestamp;                                //  1
    UserRefNum  user_ref_num;                             //  9
    Firm        firm;                                     // 13  all caps
    Symbol      symbol;                                   // 17  optional, space-filled
    AppendageLength appendage_length = 0;                 // 25
};                                                        // 27 bytes

// Type Q - Account Query Response
struct AccountQueryResponse {
    OutboundType type = OutboundType::AccountQueryResp; //  0
    Timestamp   timestamp;                              //  1
    UserRefNum  next_user_ref_num;                      //  9  next available UserRefNum
    AppendageLength appendage_length = 0;               // 13
};                                                      // 15 bytes

#pragma pack(pop)

static_assert(sizeof(SystemEvent)          == 10);
static_assert(sizeof(Accepted)             == 60);
static_assert(sizeof(Replaced)             == 64);
static_assert(sizeof(Canceled)             == 20);
static_assert(sizeof(Executed)             == 32);
static_assert(sizeof(BrokenTrade)          == 38);
static_assert(sizeof(Rejected)             == 31);
static_assert(sizeof(CancelPending)        == 15);
static_assert(sizeof(CancelReject)         == 15);
static_assert(sizeof(PriorityUpdate)       == 28);
static_assert(sizeof(Modified)             == 20);
static_assert(sizeof(Restated)             == 16);
static_assert(sizeof(MassCancelResponse)   == 27);
static_assert(sizeof(AccountQueryResponse) == 15);

} // namespace outbound

} // namespace ouch

#endif // OUCH_HPP
