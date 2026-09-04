#include <gtest/gtest.h>

extern "C" {
int rqc_test_handshake_frame_round_trip(void);
int rqc_test_malformed_handshake_frame(void);
int rqc_test_client_handshake_state(void);
int rqc_test_server_handshake_state(void);
int rqc_test_server_response_proto_ext_uses_embedded_array(void);
int rqc_test_server_rejects_invalid_response_proto_ext(void);
int rqc_test_abnormal_handshake_input(void);
int rqc_test_stream_send_flush_on_eagain(void);
int rqc_test_stream_sendv_validation_and_empty(void);
int rqc_test_stream_sendv_complete_and_fixed_pending(void);
int rqc_test_stream_sendv_dynamic_multidrain_and_close(void);
int rqc_test_fast_wakeup_dedup(void);
int rqc_test_stream_peek(void);
}

TEST(RqcUnitTest, HandshakeFrameRoundTrip)
{
    EXPECT_EQ(rqc_test_handshake_frame_round_trip(), 0);
}

TEST(RqcUnitTest, MalformedHandshakeFrame)
{
    EXPECT_EQ(rqc_test_malformed_handshake_frame(), 0);
}

TEST(RqcUnitTest, ClientHandshakeState)
{
    EXPECT_EQ(rqc_test_client_handshake_state(), 0);
}

TEST(RqcUnitTest, ServerHandshakeState)
{
    EXPECT_EQ(rqc_test_server_handshake_state(), 0);
}

TEST(RqcUnitTest, ServerResponseProtoExtUsesEmbeddedArray)
{
    EXPECT_EQ(rqc_test_server_response_proto_ext_uses_embedded_array(), 0);
}

TEST(RqcUnitTest, ServerRejectsInvalidResponseProtoExt)
{
    EXPECT_EQ(rqc_test_server_rejects_invalid_response_proto_ext(), 0);
}

TEST(RqcUnitTest, AbnormalHandshakeInput)
{
    EXPECT_EQ(rqc_test_abnormal_handshake_input(), 0);
}

TEST(RqcUnitTest, StreamSendFlushOnEagain)
{
    EXPECT_EQ(rqc_test_stream_send_flush_on_eagain(), 0);
}

TEST(RqcUnitTest, StreamSendvValidationAndEmpty)
{
    EXPECT_EQ(rqc_test_stream_sendv_validation_and_empty(), 0);
}

TEST(RqcUnitTest, StreamSendvCompleteAndFixedPending)
{
    EXPECT_EQ(rqc_test_stream_sendv_complete_and_fixed_pending(), 0);
}

TEST(RqcUnitTest, StreamSendvDynamicMultidrainAndClose)
{
    EXPECT_EQ(rqc_test_stream_sendv_dynamic_multidrain_and_close(), 0);
}

TEST(RqcUnitTest, FastWakeupDedup)
{
    EXPECT_EQ(rqc_test_fast_wakeup_dedup(), 0);
}

TEST(RqcUnitTest, StreamPeek)
{
    EXPECT_EQ(rqc_test_stream_peek(), 0);
}
