/* Copyright (C) 2007-2011 Open Information Security Foundation
 *
 * You can copy, redistribute or modify this Program under the terms of
 * the GNU General Public License version 2 as published by the Free
 * Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

/**
 * \file
 *
 * \author Victor Julien <victor@inliniac.net>
 * \author Anoop Saldanha <anoopsaldanha@gmail.com>
 *
 * Generic App-layer functions
 */

#include "suricata-common.h"

#include "app-layer.h"
#include "app-layer-parser.h"
#include "app-layer-protos.h"
#include "app-layer-detect-proto.h"
#include "stream-tcp-reassemble.h"
#include "stream-tcp-private.h"
#include "stream-tcp-inline.h"
#include "flow.h"
#include "flow-util.h"

#include "util-debug.h"
#include "util-print.h"
#include "util-profiling.h"
#include "util-validate.h"
#include "decode-events.h"

/**
 * \brief This is for the app layer in general and it contains per thread
 *        context relevant to both the alpd and alp.
 */
typedef struct AppLayerCtxThread_ {
    /* App layer protocol detection thread context, from AlpdGetCtxThread(). */
    void *alpd_tctx;
    /* App layer parser thread context, from AlpGetCtxThread(). */
    void *alp_tctx;

#ifdef PROFILING
    uint64_t ticks_start;
    uint64_t ticks_end;
    uint64_t ticks_spent;
    uint16_t alproto;
    uint64_t proto_detect_ticks_start;
    uint64_t proto_detect_ticks_end;
    uint64_t proto_detect_ticks_spent;
#endif
} AppLayerCtxThread;

int AppLayerSetup(void)
{
    SCEnter();

    AlpdSetup();
    AlpSetup();

    AlpRegisterProtocolParsers();
    AlpdPrepareState();

    SCReturnInt(0);
}

void *AppLayerGetCtxThread(void)
{
    SCEnter();

    AppLayerCtxThread *app_tctx = SCMalloc(sizeof(*app_tctx));
    if (app_tctx == NULL)
        goto error;
    memset(app_tctx, 0, sizeof(*app_tctx));

    if ((app_tctx->alpd_tctx = AlpdGetCtxThread()) == NULL)
        goto error;
    if ((app_tctx->alp_tctx = AlpGetCtxThread()) == NULL)
        goto error;

    goto done;
 error:
    AppLayerDestroyCtxThread(app_tctx);
    app_tctx = NULL;
 done:
    SCReturnPtr(app_tctx, "void *");
}

void AppLayerDestroyCtxThread(void *tctx)
{
    SCEnter();

    AppLayerCtxThread *app_tctx = (AppLayerCtxThread *)tctx;

    if (app_tctx->alpd_tctx != NULL)
        AlpdDestroyCtxThread(app_tctx->alpd_tctx);
    if (app_tctx->alp_tctx != NULL)
        AlpDestroyCtxThread(app_tctx->alp_tctx);
    SCFree(app_tctx);

    SCReturn;
}

int AppLayerHandleTCPData(ThreadVars *tv, TcpReassemblyThreadCtx *ra_ctx,
                          Packet *p, Flow *f,
                          TcpSession *ssn, TcpStream *stream,
                          uint8_t *data, uint32_t data_len,
                          uint8_t flags)
{
    SCEnter();

    DEBUG_ASSERT_FLOW_LOCKED(f);

    AppLayerCtxThread *app_tctx = ra_ctx->app_tctx;
    uint16_t *alproto;
    uint16_t *alproto_otherdir;
    uint8_t dir;
    uint32_t data_al_so_far;
    int r = 0;
    uint8_t first_data_dir;

    SCLogDebug("data_len %u flags %02X", data_len, flags);
    if (f->flags & FLOW_NO_APPLAYER_INSPECTION) {
        SCLogDebug("FLOW_AL_NO_APPLAYER_INSPECTION is set");
        goto end;
    }

    if (flags & STREAM_TOSERVER) {
        alproto = &f->alproto_ts;
        alproto_otherdir = &f->alproto_tc;
        dir = 0;
    } else {
        alproto = &f->alproto_tc;
        alproto_otherdir = &f->alproto_ts;
        dir = 1;
    }

    /* if we don't know the proto yet and we have received a stream
     * initializer message, we run proto detection.
     * We receive 2 stream init msgs (one for each direction) but we
     * only run the proto detection once. */
    if (*alproto == ALPROTO_UNKNOWN && (flags & STREAM_GAP)) {
        StreamTcpSetStreamFlagAppProtoDetectionCompleted(stream);
        StreamTcpSetSessionNoReassemblyFlag(ssn, dir);
        SCLogDebug("ALPROTO_UNKNOWN flow %p, due to GAP in stream start", f);
    } else if (*alproto == ALPROTO_UNKNOWN && (flags & STREAM_START)) {
        if (data_len == 0)
            data_al_so_far = 0;
        else
            data_al_so_far = f->data_al_so_far[dir];

        SCLogDebug("Stream initializer (len %" PRIu32 ")", data_len);
#ifdef PRINT
        if (data_len > 0) {
            printf("=> Init Stream Data (app layer) -- start %s%s\n",
                   flags & STREAM_TOCLIENT ? "toclient" : "",
                   flags & STREAM_TOSERVER ? "toserver" : "");
            PrintRawDataFp(stdout, data, data_len);
            printf("=> Init Stream Data -- end\n");
        }
#endif

        PACKET_PROFILING_APP_PD_START(app_tctx);
        *alproto = AlpdGetProto(app_tctx->alpd_tctx,
                                f,
                                data, data_len,
                                IPPROTO_TCP, flags);
        PACKET_PROFILING_APP_PD_END(app_tctx);

        if (*alproto != ALPROTO_UNKNOWN) {
            if (*alproto_otherdir != ALPROTO_UNKNOWN && *alproto_otherdir != *alproto) {
                AppLayerDecoderEventsSetEventRaw(p->app_layer_events,
                                                 APPLAYER_MISMATCH_PROTOCOL_BOTH_DIRECTIONS);
                /* it indicates some data has already been sent to the parser */
                if (ssn->data_first_seen_dir == APP_LAYER_DATA_ALREADY_SENT_TO_APP_LAYER) {
                    f->alproto = *alproto = *alproto_otherdir;
                } else {
                    if (flags & STREAM_TOCLIENT)
                        f->alproto = *alproto_otherdir = *alproto;
                    else
                        f->alproto = *alproto = *alproto_otherdir;
                }
            }

            f->alproto = *alproto;
            StreamTcpSetStreamFlagAppProtoDetectionCompleted(stream);

            /* if we have seen data from the other direction first, send
             * data for that direction first to the parser.  This shouldn't
             * be an issue, since each stream processing happens
             * independently of the other stream direction.  At this point of
             * call, you need to know that this function's already being
             * called by the very same StreamReassembly() function that we
             * will now call shortly for the opposing direction. */
            if ((ssn->data_first_seen_dir & (STREAM_TOSERVER | STREAM_TOCLIENT)) &&
                !(flags & ssn->data_first_seen_dir)) {
                TcpStream *opposing_stream = NULL;
                if (stream == &ssn->client) {
                    opposing_stream = &ssn->server;
                    if (StreamTcpInlineMode()) {
                        p->flowflags &= ~FLOW_PKT_TOSERVER;
                        p->flowflags |= FLOW_PKT_TOCLIENT;
                    } else {
                        p->flowflags &= ~FLOW_PKT_TOCLIENT;
                        p->flowflags |= FLOW_PKT_TOSERVER;
                    }
                } else {
                    opposing_stream = &ssn->client;
                    if (StreamTcpInlineMode()) {
                        p->flowflags &= ~FLOW_PKT_TOCLIENT;
                        p->flowflags |= FLOW_PKT_TOSERVER;
                    } else {
                        p->flowflags &= ~FLOW_PKT_TOSERVER;
                        p->flowflags |= FLOW_PKT_TOCLIENT;
                    }
                }
                int ret;
                if (StreamTcpInlineMode()) {
                    ret = StreamTcpReassembleInlineAppLayer(tv, ra_ctx, ssn,
                                                            opposing_stream, p);
                } else {
                    ret = StreamTcpReassembleAppLayer(tv, ra_ctx, ssn,
                                                      opposing_stream, p);
                }
                if (stream == &ssn->client) {
                    if (StreamTcpInlineMode()) {
                        p->flowflags &= ~FLOW_PKT_TOCLIENT;
                        p->flowflags |= FLOW_PKT_TOSERVER;
                    } else {
                        p->flowflags &= ~FLOW_PKT_TOSERVER;
                        p->flowflags |= FLOW_PKT_TOCLIENT;
                    }
                } else {
                    if (StreamTcpInlineMode()) {
                        p->flowflags &= ~FLOW_PKT_TOSERVER;
                        p->flowflags |= FLOW_PKT_TOCLIENT;
                    } else {
                        p->flowflags &= ~FLOW_PKT_TOCLIENT;
                        p->flowflags |= FLOW_PKT_TOSERVER;
                    }
                }
                if (ret < 0) {
                    FlowSetSessionNoApplayerInspectionFlag(f);
                    StreamTcpSetStreamFlagAppProtoDetectionCompleted(&ssn->client);
                    StreamTcpSetStreamFlagAppProtoDetectionCompleted(&ssn->server);
                    goto failure;
                }
            }

            /* if the parser operates such that it needs to see data from
             * a particular direction first, we check if we have seen
             * data from that direction first for the flow.  IF it is not
             * the same, we set an event and exit.
             *
             * \todo We need to figure out a more robust solution for this,
             *       as this can lead to easy evasion tactics, where the
             *       attackeer can first send some dummy data in the wrong
             *       direction first to mislead our proto detection process.
             *       While doing this we need to update the parsers as well,
             *       since the parsers must be robust to see such wrong
             *       direction data.
             *       Either ways the moment we see the
             *       APPLAYER_WRONG_DIRECTION_FIRST_DATA event set for the
             *       flow, it shows something's fishy.
             */
            if (ssn->data_first_seen_dir != APP_LAYER_DATA_ALREADY_SENT_TO_APP_LAYER) {
                first_data_dir = AlpGetFirstDataDir(f->proto, *alproto);

                if (first_data_dir && !(first_data_dir & ssn->data_first_seen_dir)) {
                    AppLayerDecoderEventsSetEventRaw(p->app_layer_events,
                                                     APPLAYER_WRONG_DIRECTION_FIRST_DATA);
                    FlowSetSessionNoApplayerInspectionFlag(f);
                    StreamTcpSetStreamFlagAppProtoDetectionCompleted(&ssn->server);
                    StreamTcpSetStreamFlagAppProtoDetectionCompleted(&ssn->client);
                    /* Set a value that is neither STREAM_TOSERVER, nor STREAM_TOCLIENT */
                    ssn->data_first_seen_dir = APP_LAYER_DATA_ALREADY_SENT_TO_APP_LAYER;
                    goto failure;
                }
                /* This can happen if the current direction is not the
                 * right direction, and the data from the other(also
                 * the right direction) direction is available to be sent
                 * to the app layer, but it is not ack'ed yet and hence
                 * the forced call to STreamTcpAppLayerReassemble still
                 * hasn't managed to send data from the other direction
                 * to the app layer. */
                if (first_data_dir && !(first_data_dir & flags)) {
                    BUG_ON(*alproto_otherdir != ALPROTO_UNKNOWN);
                    FlowCleanupAppLayer(f);
                    f->alproto = *alproto = ALPROTO_UNKNOWN;
                    StreamTcpResetStreamFlagAppProtoDetectionCompleted(stream);
                    FLOW_RESET_PP_DONE(f, flags);
                    FLOW_RESET_PM_DONE(f, flags);
                    goto failure;
                }
            }

            /* Set a value that is neither STREAM_TOSERVER, nor STREAM_TOCLIENT */
            ssn->data_first_seen_dir = APP_LAYER_DATA_ALREADY_SENT_TO_APP_LAYER;

            PACKET_PROFILING_APP_START(app_tctx, *alproto);
            r = AlpParseL7Data(app_tctx->alp_tctx, f, *alproto, flags, data + data_al_so_far, data_len - data_al_so_far);
            PACKET_PROFILING_APP_END(app_tctx, *alproto);
            f->data_al_so_far[dir] = 0;
        } else {
            if (*alproto_otherdir != ALPROTO_UNKNOWN) {
                first_data_dir = AlpGetFirstDataDir(f->proto, *alproto_otherdir);

                /* this would handle this test case -
                 * http parser which says it wants to see toserver data first only.
                 * tcp handshake
                 * toclient data first received. - RUBBISH DATA which
                 *                                 we don't detect as http
                 * toserver data next sent - we detect this as http.
                 * at this stage we see that toclient is the first data seen
                 * for this session and we try and redetect the app protocol,
                 * but we are unable to detect the app protocol like before.
                 * But since we have managed to detect the protocol for the
                 * other direction as http, we try to use that.  At this
                 * stage we check if the direction of this stream matches
                 * to that acceptable by the app parser.  If it is not the
                 * acceptable direction we error out.
                 */
                if ((ssn->data_first_seen_dir != APP_LAYER_DATA_ALREADY_SENT_TO_APP_LAYER) &&
                    (first_data_dir) && !(first_data_dir & flags))
                    {
                        FlowSetSessionNoApplayerInspectionFlag(f);
                        StreamTcpSetStreamFlagAppProtoDetectionCompleted(&ssn->server);
                        StreamTcpSetStreamFlagAppProtoDetectionCompleted(&ssn->client);
                        goto failure;
                    }

                if (data_len > 0)
                    ssn->data_first_seen_dir = APP_LAYER_DATA_ALREADY_SENT_TO_APP_LAYER;

                PACKET_PROFILING_APP_START(app_tctx, *alproto_otherdir);
                r = AlpParseL7Data(app_tctx->alp_tctx, f, *alproto_otherdir, flags,
                                  data + data_al_so_far, data_len - data_al_so_far);
                PACKET_PROFILING_APP_END(app_tctx, *alproto_otherdir);
                if (FLOW_IS_PM_DONE(f, flags) && FLOW_IS_PP_DONE(f, flags)) {
                    AppLayerDecoderEventsSetEventRaw(p->app_layer_events,
                                                     APPLAYER_DETECT_PROTOCOL_ONLY_ONE_DIRECTION);
                    StreamTcpSetStreamFlagAppProtoDetectionCompleted(stream);
                    f->data_al_so_far[dir] = 0;
                } else {
                    f->data_al_so_far[dir] = data_len;
                }
            } else {
                if (FLOW_IS_PM_DONE(f, STREAM_TOSERVER) && FLOW_IS_PP_DONE(f, STREAM_TOSERVER) &&
                    FLOW_IS_PM_DONE(f, STREAM_TOCLIENT) && FLOW_IS_PP_DONE(f, STREAM_TOCLIENT)) {
                    FlowSetSessionNoApplayerInspectionFlag(f);
                    StreamTcpSetStreamFlagAppProtoDetectionCompleted(&ssn->server);
                    StreamTcpSetStreamFlagAppProtoDetectionCompleted(&ssn->client);
                    ssn->data_first_seen_dir = APP_LAYER_DATA_ALREADY_SENT_TO_APP_LAYER;
                }
            }
        }
    } else {
        SCLogDebug("stream data (len %" PRIu32 " alproto "
                   "%"PRIu16" (flow %p)", data_len, f->alproto, f);
#ifdef PRINT
        if (data_len > 0) {
            printf("=> Stream Data (app layer) -- start %s%s\n",
                   flags & STREAM_TOCLIENT ? "toclient" : "",
                   flags & STREAM_TOSERVER ? "toserver" : "");
            PrintRawDataFp(stdout, data, data_len);
            printf("=> Stream Data -- end\n");
        }
#endif
        /* if we don't have a data object here we are not getting it
         * a start msg should have gotten us one */
        if (f->alproto != ALPROTO_UNKNOWN) {
            PACKET_PROFILING_APP_START(app_tctx, f->alproto);
            r = AlpParseL7Data(app_tctx->alp_tctx, f, f->alproto, flags, data, data_len);
            PACKET_PROFILING_APP_END(app_tctx, f->alproto);
        } else {
            SCLogDebug(" smsg not start, but no l7 data? Weird");
        }
    }

    goto end;
 failure:
    r = -1;
 end:
    SCReturnInt(r);
}

int AppLayerHandleTCPMsg(StreamMsg *smsg)
{
    SCEnter();

    TcpSession *ssn;
    StreamMsg *cur;

#ifdef PRINT
    printf("=> Stream Data (raw reassembly) -- start %s%s\n",
           smsg->flags & STREAM_TOCLIENT ? "toclient" : "",
           smsg->flags & STREAM_TOSERVER ? "toserver" : "");
    PrintRawDataFp(stdout, smsg->data.data, smsg->data.data_len);
    printf("=> Stream Data -- end\n");
#endif
    SCLogDebug("smsg %p", smsg);
    BUG_ON(smsg->flow == NULL);

    ssn = smsg->flow->protoctx;
    if (ssn != NULL) {
        SCLogDebug("storing smsg %p in the tcp session", smsg);

        /* store the smsg in the tcp stream */
        if (smsg->flags & STREAM_TOSERVER) {
            SCLogDebug("storing smsg in the to_server");

            /* put the smsg in the stream list */
            if (ssn->toserver_smsg_head == NULL) {
                ssn->toserver_smsg_head = smsg;
                ssn->toserver_smsg_tail = smsg;
                smsg->next = NULL;
                smsg->prev = NULL;
            } else {
                cur = ssn->toserver_smsg_tail;
                cur->next = smsg;
                smsg->prev = cur;
                smsg->next = NULL;
                ssn->toserver_smsg_tail = smsg;
            }
        } else {
            SCLogDebug("storing smsg in the to_client");

            /* put the smsg in the stream list */
            if (ssn->toclient_smsg_head == NULL) {
                ssn->toclient_smsg_head = smsg;
                ssn->toclient_smsg_tail = smsg;
                smsg->next = NULL;
                smsg->prev = NULL;
            } else {
                cur = ssn->toclient_smsg_tail;
                cur->next = smsg;
                smsg->prev = cur;
                smsg->next = NULL;
                ssn->toclient_smsg_tail = smsg;
            }
        }

        FlowDeReference(&smsg->flow);
    } else { /* no ssn ptr */
        /* if there is no ssn ptr we won't
         * be inspecting this msg in detect
         * so return it to the pool. */

        FlowDeReference(&smsg->flow);

        /* return the used message to the queue */
        StreamMsgReturnToPool(smsg);
    }

    SCReturnInt(0);
}

int AppLayerHandleUdp(void *app_tctx, Packet *p, Flow *f)
{
    SCEnter();

    AppLayerCtxThread *tctx = (AppLayerCtxThread *)app_tctx;

    int r = 0;

    FLOWLOCK_WRLOCK(f);

    uint8_t flags = 0;
    if (p->flowflags & FLOW_PKT_TOSERVER) {
        flags |= STREAM_TOSERVER;
    } else {
        flags |= STREAM_TOCLIENT;
    }

    /* if we don't know the proto yet and we have received a stream
     * initializer message, we run proto detection.
     * We receive 2 stream init msgs (one for each direction) but we
     * only run the proto detection once. */
    if (f->alproto == ALPROTO_UNKNOWN && !(f->flags & FLOW_ALPROTO_DETECT_DONE)) {
        SCLogDebug("Detecting AL proto on udp mesg (len %" PRIu32 ")",
                   p->payload_len);

        PACKET_PROFILING_APP_PD_START(tctx);
        f->alproto = AlpdGetProto(tctx->alpd_tctx,
                                  f,
                                  p->payload, p->payload_len,
                                  IPPROTO_UDP, flags);
        PACKET_PROFILING_APP_PD_END(tctx);

        if (f->alproto != ALPROTO_UNKNOWN) {
            f->flags |= FLOW_ALPROTO_DETECT_DONE;

            PACKET_PROFILING_APP_START(tctx, f->alproto);
            r = AlpParseL7Data(tctx->alp_tctx,
                              f, f->alproto, flags,
                              p->payload, p->payload_len);
            PACKET_PROFILING_APP_END(tctx, f->alproto);
        } else {
            f->flags |= FLOW_ALPROTO_DETECT_DONE;
            SCLogDebug("ALPROTO_UNKNOWN flow %p", f);
        }
    } else {
        SCLogDebug("stream data (len %" PRIu32 " ), alproto "
                   "%"PRIu16" (flow %p)", p->payload_len, f->alproto, f);

        /* if we don't have a data object here we are not getting it
         * a start msg should have gotten us one */
        if (f->alproto != ALPROTO_UNKNOWN) {
            PACKET_PROFILING_APP_START(dp_ctx, f->alproto);
            r = AlpParseL7Data(tctx->alp_tctx,
                              f, f->alproto, flags,
                              p->payload, p->payload_len);
            PACKET_PROFILING_APP_END(dp_ctx, f->alproto);
        } else {
            SCLogDebug("udp session has started, but failed to detect alproto "
                       "for l7");
        }
    }

    FLOWLOCK_UNLOCK(f);
    PACKET_PROFILING_APP_STORE(tctx, p);
    SCReturnInt(r);
}

AppProto AppLayerGetProtoByName(char *alproto_name)
{
    SCEnter();
    SCReturnCT(AlpdGetProtoByName(alproto_name), "AppProto");
}

char *AppLayerGetProtoString(AppProto alproto)
{
    SCEnter();
    SCReturnCT(AlpdGetProtoString(alproto), "char *");
}

void AppLayerUnittestsRegister(void);
