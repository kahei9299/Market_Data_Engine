#define BOOST_TEST_MODULE market_data_engine_tests
#include <boost/test/unit_test.hpp>

#include "session.hpp"
#include "util.hpp"
#include "validation.hpp"

#include <boost/beast/core/flat_buffer.hpp>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <atomic>
#include <regex>
#include <string>

// --- timestamp_now tests ---

BOOST_AUTO_TEST_SUITE(timestamp)

BOOST_AUTO_TEST_CASE(format_matches_iso8601) {
    auto ts = timestamp_now();
    // e.g. 2026-11-02T04:08:46.162Z
    std::regex pattern(R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z)");
    BOOST_TEST(std::regex_match(ts, pattern));
}

BOOST_AUTO_TEST_CASE(length_is_24) {
    auto ts = timestamp_now();
    BOOST_TEST(ts.size() == 24u);
}

BOOST_AUTO_TEST_SUITE_END()

// --- validate_depth_message tests ---

BOOST_AUTO_TEST_SUITE(validation)

static const std::string kValidMessage = R"({
    "e": "depthUpdate",
    "E": 1793587539014,
    "s": "BTCUSDT",
    "U": 102171391360,
    "u": 102171391511,
    "b": [["78450.06000000","0.04581000"]],
    "a": [["78450.07000000","2.27294000"]]
})";

BOOST_AUTO_TEST_CASE(valid_message) {
    auto r = validate_depth_message(kValidMessage);
    BOOST_TEST(r.ok);
    BOOST_TEST(r.error.empty());
}

BOOST_AUTO_TEST_CASE(invalid_json) {
    auto r = validate_depth_message("not json at all");
    BOOST_TEST(!r.ok);
    BOOST_TEST(r.error.find("invalid JSON") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(empty_input) {
    auto r = validate_depth_message("");
    BOOST_TEST(!r.ok);
}

BOOST_AUTO_TEST_CASE(json_array_not_object) {
    auto r = validate_depth_message("[1, 2, 3]");
    BOOST_TEST(!r.ok);
    BOOST_TEST(r.error.find("expected JSON object") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(missing_event_type) {
    auto r = validate_depth_message(R"({
        "E": 123, "U": 1, "u": 2, "b": [], "a": []
    })");
    BOOST_TEST(!r.ok);
    BOOST_TEST(r.error.find("\"e\"") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(wrong_event_type) {
    auto r = validate_depth_message(R"({
        "e": "trade", "E": 123, "U": 1, "u": 2, "b": [], "a": []
    })");
    BOOST_TEST(!r.ok);
    BOOST_TEST(r.error.find("unexpected event type") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(missing_numeric_field_U) {
    auto r = validate_depth_message(R"({
        "e": "depthUpdate", "E": 123, "u": 2, "b": [], "a": []
    })");
    BOOST_TEST(!r.ok);
    BOOST_TEST(r.error.find("\"U\"") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(missing_bids_array) {
    auto r = validate_depth_message(R"({
        "e": "depthUpdate", "E": 123, "U": 1, "u": 2, "a": []
    })");
    BOOST_TEST(!r.ok);
    BOOST_TEST(r.error.find("\"b\"") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(missing_asks_array) {
    auto r = validate_depth_message(R"({
        "e": "depthUpdate", "E": 123, "U": 1, "u": 2, "b": []
    })");
    BOOST_TEST(!r.ok);
    BOOST_TEST(r.error.find("\"a\"") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(bids_not_array) {
    auto r = validate_depth_message(R"({
        "e": "depthUpdate", "E": 123, "U": 1, "u": 2, "b": "wrong", "a": []
    })");
    BOOST_TEST(!r.ok);
    BOOST_TEST(r.error.find("\"b\"") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(empty_arrays_are_valid) {
    auto r = validate_depth_message(R"({
        "e": "depthUpdate", "E": 123, "s": "BTCUSDT", "U": 1, "u": 2, "b": [], "a": []
    })");
    BOOST_TEST(r.ok);
}

BOOST_AUTO_TEST_SUITE_END()

// --- bounded buffer test ---

BOOST_AUTO_TEST_SUITE(buffer)

BOOST_AUTO_TEST_CASE(flat_buffer_respects_max_size) {
    boost::beast::flat_buffer buf(64);
    BOOST_TEST(buf.max_size() == 64u);
}

BOOST_AUTO_TEST_SUITE_END()

// --- TLS hostname verification ---

BOOST_AUTO_TEST_SUITE(tls)

BOOST_AUTO_TEST_CASE(ssl_set1_host_configures_verification) {
    ssl::context ctx(ssl::context::tlsv12_client);
    SSL* handle = SSL_new(ctx.native_handle());
    BOOST_REQUIRE(handle != nullptr);

    BOOST_TEST(SSL_set_tlsext_host_name(handle, "example.com") == 1);
    BOOST_TEST(SSL_set1_host(handle, "example.com") == 1);

    // Confirm the verification parameter object exists and is configured.
    X509_VERIFY_PARAM* param = SSL_get0_param(handle);
    BOOST_REQUIRE(param != nullptr);

    // SSL_set1_host registers the hostname for verification — OpenSSL will
    // check the cert's SAN/CN during handshake. The hostname_mismatch test
    // below proves this rejects actual mismatches end-to-end.

    SSL_free(handle);
}

// Self-signed cert for "testserver.local" — used only in tests.
static constexpr char kTestCert[] = R"(-----BEGIN CERTIFICATE-----
MIIDNDCCAhygAwIBAgIUM5Nzg/6ZexgbDDGoVaabU8j6oHEwDQYJKoZIhvcNAQEL
BQAwGzEZMBcGA1UEAwwQdGVzdHNlcnZlci5sb2NhbDAeFw0yNjExMDMwNTU5NDda
Fw0zNjEwMzEwNTU5NDdaMBsxGTAXBgNVBAMMEHRlc3RzZXJ2ZXIubG9jYWwwggEi
MA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQCyB1rPZ11kAmXDe9dLR3qX7F+1
kLcMzswTwlFwIdxEVgftEN26OgtVZei6/R2tPPZZ7P+H962aC+r+E1r2aqtuP/hn
L8YwjljuAOQ/ldN+kPxbuIsFhJ8lq8VNXNxVd64pmxjkHU+81qxh1V0ygon0HbLP
0fvxGyqUeQNvBhG5g4R8NZyY42ML5rwrLD2Ibs7v8mZulORoc4ChNyQX2U/mvISw
0kHym6Szt4GxXyl3McJB7ZomjhsKZlwoZpRv/4AAcrZ9mCc3WSAQGIHPl1+HdUC/
wKL0rWoRiVCzNGYAZ4eqrFBE7z45RLzE23nuv9ZTV0B6c+I8FFCsdb30s5XZAgMB
AAGjcDBuMB0GA1UdDgQWBBTqR+enN7Pz8rQ42j/YL6m34l+QETAfBgNVHSMEGDAW
gBTqR+enN7Pz8rQ42j/YL6m34l+QETAPBgNVHRMBAf8EBTADAQH/MBsGA1UdEQQU
MBKCEHRlc3RzZXJ2ZXIubG9jYWwwDQYJKoZIhvcNAQELBQADggEBAFSghbhLZ9DI
doo2QFBuJgbG3F11Z8Vi+mkC3YK+QVDXFMCN4VtF2Kzq2Hw0EpTD28RXt8Zf8AwD
cNF766pUs2YLQ/8Vts+LZABLI2greU9OwMI6b3KfxYYdSH1rGntMq7nRuoIlwz1d
FnKF30kb7SFVCO2jF4TXlJ0kv+/g4ug+jVT7VBRAvhfjmBUfLgEyBw1fAZ979h36
JOBXZ3rsYrmzF/uDLaIWLeoir1BU5ZMBIacVMQ90fftDGJu7Ftdidp2kozwWbOR9
L224ltN70rW/4iDnqJnkZpp1UZFgq9FCBIazpQvsbA0Er1wmXsbVUCnTWDx3b/x6
XgAcuI2ggVI=
-----END CERTIFICATE-----)";

static constexpr char kTestKey[] = R"(-----BEGIN PRIVATE KEY-----
MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQCyB1rPZ11kAmXD
e9dLR3qX7F+1kLcMzswTwlFwIdxEVgftEN26OgtVZei6/R2tPPZZ7P+H962aC+r+
E1r2aqtuP/hnL8YwjljuAOQ/ldN+kPxbuIsFhJ8lq8VNXNxVd64pmxjkHU+81qxh
1V0ygon0HbLP0fvxGyqUeQNvBhG5g4R8NZyY42ML5rwrLD2Ibs7v8mZulORoc4Ch
NyQX2U/mvISw0kHym6Szt4GxXyl3McJB7ZomjhsKZlwoZpRv/4AAcrZ9mCc3WSAQ
GIHPl1+HdUC/wKL0rWoRiVCzNGYAZ4eqrFBE7z45RLzE23nuv9ZTV0B6c+I8FFCs
db30s5XZAgMBAAECggEABMifzwzm/IX3+A2+1j4AdE6cr8NIt+Zio3FIQOwYABoT
snIOky92GIBZhZTBlGMb94SfxSaJmqcRVGxuuEpbeuXrJdKpz6ozh4msmGzNNwxs
S4ak7awHnrqD3bVOElnZ6zJIs/B5sQhPr31NOMs7K70UjfMFgxAIz96EkhompcZw
cta6Dl4u6eX4ReR834lIVfcineLPYKSIZAYz2lk7s1CxBXx6V+3wN+68du6/dtPq
q/L74+t3LF+KC5ngcvVeLocOif0SwoNvL97giyxce4yspIJwnCh16uGlLRfoDtWP
oweATF3J547KWA3F3t3IU1I1/go65FrUhWXvIYpPZQKBgQDV2oPWUt9xbRW93HVS
oy9cN6W5Pc470gatvbTjY8hNeSiQ/y6uZu5JA74bmk+sqyB+HyDCXDTQMDP18lyc
t1hRK8+6U/knn+RhC5e7biBESZoRf3/TqsMZqWab5mE2qwKRcU3pmdgnl2I4gdTb
700HDhLLJoX4ghHY32akV+9+wwKBgQDVHWFmKztvWvHSkg53LQn4R9vIhHK9mcgc
zVN1+YqtvelhtvH04rbJK9ZzqGbMYJULwEm9ozEn/IvjNeenEFbFmLVwBHZUsb15
xrXru+aC56C3lUikdmOLFlrPu15Iqn4K7Sx9qAZUzRrKNVgJVbRYMiVNmtap0sNl
A8xMIgIHMwKBgC+0BbKaW+N2XOI0OiuL4TWl6B7RLZ7kc77v0Vjj2juF6kml0qju
pvq8jfykSF+Q8LeRhAI3BUTna7/SJ1yB/X5l0nmS38I6BXysVFVn9GSnMmny0fXK
Yqjnw87E7ix+qxz8dE/GlJQITc2jEHNbBwiq2ABfAIV1pjuFYeUJBJB3AoGAIA51
Uir6m7twWJ5DssWBlGkqtDTIFaEGKt7vyPUqWQwSCPK93nojyIrhjTAtllyIzuWz
seTURhGE8T5wg/6/LqXT1O+25PJ/b8vmHXl8KQCyeirobGSqgIdyTYD/o6qRnw8h
/BzEJwlbGEOB5JWRtRqF5fB+jbj4Tz+NAX60E2UCgYEAxx3Zz3KVodhrARByydXM
tYbCk+kpZ0FQblj+Ctj5shiAtEwxZxbj7UoXduPJFp+qJZWAaHGu0P8aEcNLNobU
xEpps7lFh3NB14EIreRcOkoYQkP8Wx0JvsaF7ckii/dGvL9nLCpxBLQqRIGgWl6p
01RlX7VXAT2ogYGoET1dlyc=
-----END PRIVATE KEY-----)";

BOOST_AUTO_TEST_CASE(hostname_mismatch_rejected_by_local_server) {
    // Local TLS server with cert for "testserver.local".
    // Session connects to "localhost" — SSL_set1_host("localhost") won't
    // match the cert's SAN "testserver.local", so the handshake must fail.

    net::io_context ioc;

    // Server context: load the test cert/key.
    ssl::context server_ctx(ssl::context::tlsv12_server);
    server_ctx.use_certificate_chain(
        net::buffer(kTestCert, std::strlen(kTestCert)));
    server_ctx.use_private_key(
        net::buffer(kTestKey, std::strlen(kTestKey)), ssl::context::pem);

    // Bind to an ephemeral port on localhost.
    beast::error_code bind_ec;
    tcp::acceptor acceptor(ioc);
    acceptor.open(tcp::v4(), bind_ec);
    if (bind_ec) {
        BOOST_WARN_MESSAGE(false, "skipped: cannot open socket");
        return;
    }
    acceptor.bind(tcp::endpoint(net::ip::make_address("127.0.0.1"), 0), bind_ec);
    if (bind_ec) {
        BOOST_WARN_MESSAGE(false, "skipped: cannot bind (sandbox?)");
        return;
    }
    acceptor.listen(1, bind_ec);
    if (bind_ec) {
        BOOST_WARN_MESSAGE(false, "skipped: cannot listen");
        return;
    }
    auto port = std::to_string(acceptor.local_endpoint().port());

    // Accept one connection and attempt server-side TLS handshake.
    ssl::stream<tcp::socket> server_stream(ioc, server_ctx);
    acceptor.async_accept(server_stream.next_layer(),
        [&server_stream](beast::error_code ec) {
            if (!ec) {
                server_stream.async_handshake(ssl::stream_base::server,
                    [](beast::error_code) {});
            }
        });

    // Client context: trust our self-signed cert's CA.
    ssl::context client_ctx(ssl::context::tlsv12_client);
    client_ctx.add_certificate_authority(
        net::buffer(kTestCert, std::strlen(kTestCert)));
    client_ctx.set_verify_mode(ssl::verify_peer);

    // Connect as "localhost" — cert says "testserver.local" → mismatch.
    std::atomic<bool> error_fired{false};
    std::string error_stage;
    auto session = std::make_shared<Session>(
        ioc, client_ctx, "localhost", port, "/",
        [&error_fired, &error_stage](beast::error_code, char const* what) {
            error_fired = true;
            error_stage = what;
        });
    session->run();

    ioc.run();

    BOOST_TEST(error_fired.load());
    BOOST_TEST(error_stage == "ssl_handshake");
}

BOOST_AUTO_TEST_SUITE_END()

// --- Session lifecycle tests ---

BOOST_AUTO_TEST_SUITE(session_lifecycle)

BOOST_AUTO_TEST_CASE(stop_during_resolve_does_not_advance) {
    net::io_context ioc;
    ssl::context ctx(ssl::context::tlsv12_client);

    std::atomic<bool> error_called{false};
    auto session = std::make_shared<Session>(
        ioc, ctx, "nonexistent.test.invalid", "9443", "/ws/test",
        [&error_called](beast::error_code, char const*) {
            error_called = true;
        });

    session->run();
    session->stop();
    ioc.run();

    // Connection must not have advanced — stats show never connected.
    auto const& stats = session->stats();
    BOOST_TEST(stats.messages_received == 0u);
    BOOST_TEST(
        (stats.connected_at == std::chrono::steady_clock::time_point{}));
}

BOOST_AUTO_TEST_CASE(stop_before_run_is_safe) {
    net::io_context ioc;
    ssl::context ctx(ssl::context::tlsv12_client);

    auto session = std::make_shared<Session>(
        ioc, ctx, "example.com", "443", "/");

    session->stop();
    ioc.run();

    auto const& stats = session->stats();
    BOOST_TEST(stats.messages_received == 0u);
    BOOST_TEST(stats.bytes_received == 0u);
}

BOOST_AUTO_TEST_CASE(error_callback_fires_on_dns_failure) {
    net::io_context ioc;
    ssl::context ctx(ssl::context::tlsv12_client);

    std::atomic<bool> error_called{false};
    std::string error_stage;
    auto session = std::make_shared<Session>(
        ioc, ctx, "nonexistent.test.invalid", "9443", "/ws/test",
        [&error_called, &error_stage](beast::error_code, char const* what) {
            error_called = true;
            error_stage = what;
        });

    session->run();
    ioc.run();

    BOOST_TEST(error_called.load());
    BOOST_TEST(error_stage == "resolve");
}

BOOST_AUTO_TEST_CASE(stats_zero_when_never_connected) {
    net::io_context ioc;
    ssl::context ctx(ssl::context::tlsv12_client);

    auto session = std::make_shared<Session>(
        ioc, ctx, "nonexistent.test.invalid", "9443", "/ws/test");

    session->run();
    session->stop();
    ioc.run();

    auto const& stats = session->stats();
    BOOST_TEST(stats.messages_received == 0u);
    BOOST_TEST(stats.bytes_received == 0u);
    BOOST_TEST(stats.invalid_messages == 0u);
    BOOST_TEST(
        (stats.connected_at == std::chrono::steady_clock::time_point{}));
}

BOOST_AUTO_TEST_SUITE_END()
