// Observable HTTP regression coverage for transport-level 413 rendering. The server must
// distinguish a payload rejected before routing from an application-defined structured 413.

#include "serve/http_server.h"
#include "serve/openai_schema.h"
#include "serve/request.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <iostream>
#include <string>
#include <thread>

namespace {

using Json = nlohmann::json;
using namespace ninfer::serve;

int fail(const std::string& message) {
    std::cerr << "FAIL: " << message << '\n';
    return 1;
}

int check(bool condition, const std::string& message) { return condition ? 0 : fail(message); }

} // namespace

int main() {
    httplib::Server server;
    server.set_payload_max_length(32);
    configure_http_error_handler(server);

    ApiError media_error;
    media_error.status  = 413;
    media_error.type    = "invalid_request_error";
    media_error.code    = "media_budget_exceeded";
    media_error.param   = "messages";
    media_error.message = "image exceeds the configured media budget";
    const std::string expected_media_body = make_error_body(media_error);

    server.Post("/application-413",
                [&](const httplib::Request&, httplib::Response& response) {
                    response.status = media_error.status;
                    response.set_content(expected_media_body, "application/json");
                });
    server.Post("/payload-413",
                [](const httplib::Request&, httplib::Response& response) {
                    response.set_content("route reached", "text/plain");
                });

    const int port = server.bind_to_any_port("127.0.0.1");
    if (port <= 0) { return fail("bind test server"); }
    std::thread server_thread([&] { server.listen_after_bind(); });
    server.wait_until_ready();

    int failures = 0;
    httplib::Client client("127.0.0.1", port);

    const auto application_response = client.Post("/application-413", "{}", "application/json");
    failures += check(application_response && application_response->status == 413,
                      "application response status");
    if (application_response) {
        failures += check(application_response->body == expected_media_body,
                          "application 413 body remains intact");
        const Json error = Json::parse(application_response->body).at("error");
        failures += check(error.at("code") == "media_budget_exceeded",
                          "application 413 code remains intact");
    }

    const std::string oversized_body(64, 'x');
    const auto payload_response =
        client.Post("/payload-413", oversized_body, "application/json");
    failures += check(payload_response && payload_response->status == 413,
                      "payload response status");
    if (payload_response) {
        const Json error = Json::parse(payload_response->body).at("error");
        failures += check(error.at("code") == "request_too_large",
                          "payload 413 uses transport error code");
        failures += check(error.at("message") ==
                              "request body exceeds the configured payload limit",
                          "payload 413 uses transport error message");
    }

    server.stop();
    server_thread.join();
    return failures;
}
