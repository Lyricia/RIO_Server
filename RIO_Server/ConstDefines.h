#pragma once
constexpr auto MAX_THREAD = 1;

constexpr auto MAX_BUFFER = 128;
constexpr auto VIEW_RANGE = 7;

constexpr auto MAX_USER = 10000;

constexpr int NUMPIECE = 1024;
constexpr size_t BUFPIECESIZE = 32;
constexpr int SESSION_BUFFER_SIZE = NUMPIECE * BUFPIECESIZE;
constexpr int SEND_BUFFER_OFFSET = 256;

constexpr auto MAX_RIO_RESULTS = 1024;
constexpr auto MAX_SEND_RQ_SIZE_PER_SOCKET = 2048;
constexpr auto MAX_RECV_RQ_SIZE_PER_SOCKET = 2048;
constexpr auto CQ_SIZE_FACTOR = 5000;
constexpr auto MAX_CQ_SIZE_PER_RIO_THREAD = (MAX_SEND_RQ_SIZE_PER_SOCKET + MAX_SEND_RQ_SIZE_PER_SOCKET) * CQ_SIZE_FACTOR;

constexpr int MAX_ABORT_COUNT = 30;

constexpr int MAX_POST_DEFERRED_MSG_COUNT = 50;
constexpr int MIN_POST_TIME = 10;


constexpr int ZONE_HEIGHT_SIZE = 20;
constexpr int ZONE_WIDTH_SIZE = 20;
constexpr int ZONE_ONELINE_SIZE = (WORLD_WIDTH / ZONE_WIDTH_SIZE);


constexpr int epoch_freq = 40;
constexpr int empty_freq = 1000;

thread_local int tl_idx = -1;

RIO_EXTENSION_FUNCTION_TABLE gRIO = { 0, };

enum EVENT_TYPE { EV_RECV, EV_SEND, EV_MOVE, EV_PLAYER_MOVE_NOTIFY, EV_MOVE_TARGET, EV_ATTACK, EV_HEAL };
