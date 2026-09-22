// Included inside the collector probe namespace to reuse its native counter model.
struct SessionClock final : MetricClockTransport {
    FakeTransport &samples;
    uint64_t reads{};
    explicit SessionClock(FakeTransport &s) : samples(s) {}
    Json clockPair() override {
        samples.trace.push_back({"clock_pair"});
        samples.fail("clock");
        for (const auto &[id, item] : samples.owned)
            if (item.state == "begun")
                throw std::runtime_error("Clock during begun sample");
        const auto index = reads++;
        return {{"maximum_ns", 1000000},
                {"frequency_hz", 1000000000},
                {"status", 0},
                {"gpu_ns", 100 + index * 10},
                {"cpu_ns", 1000 + index * 10}};
    }
};
struct SessionLock final : MetricPriorityLock {
    FakeTransport &samples;
    bool &deviceClosed;
    uint64_t depth{};
    bool done{};
    SessionLock(FakeTransport &s, bool &closed) : samples(s), deviceClosed(closed) {}
    void setPriority(uint32_t value) override { samples.trace.push_back({"priority", value}); }
    bool acquire() override {
        samples.trace.push_back({"acquire"});
        ++depth;
        return true;
    }
    void release() override {
        samples.trace.push_back({"release_lock"});
        if (depth)
            --depth;
    }
    void close() override {
        samples.trace.push_back({"close_lock", deviceClosed, depth});
        done = true;
        depth = 0;
    }
    bool closed() const override { return done; }
    Json audit() const override { return {{"depth", depth}, {"closed", done}}; }
};
Json iterationSession(const Json &job) {
    FakeTransport samples;
    samples.supported = job.value("supported", true);
    samples.readyAfter = job.value("ready_after", 0u);
    SessionClock clock(samples);
    bool deviceClosed = false;
    unsigned closeFailures = job.value("close_failures", 0u);
    Json selected, steps = Json::array();
    SessionLock *lock = nullptr;
    std::string fault;
    uint64_t iteration{};
    MdIterationClient client{samples,
                             clock,
                             job.at("catalog"),
                             [&](const std::string &name) {
                                 samples.trace.push_back({"select", name});
                                 samples.fail("select");
                                 for (const auto &set : job["catalog"]["sets"])
                                     if (set["name"] == name)
                                         selected = set;
                             },
                             [&] { return uint64_t(samples.owned.size()); },
                             [&] {
                                 samples.trace.push_back({"close_device", lock ? lock->depth : 0});
                                 if (closeFailures) {
                                     --closeFailures;
                                     throw std::runtime_error("native close");
                                 }
                                 deviceClosed = true;
                                 samples.owned.clear();
                                 samples.cached.clear();
                             },
                             [&] { return deviceClosed; }};
    MdIterationTransport session(
        client, 0x12345678, job.at("symbols"),
        [&](MdIterationTransport &transport, uint32_t pass, const Json &ranges) {
            const auto sample = iteration++;
            for (size_t i = 0; i < ranges.size(); ++i) {
                if (fault == "omit" && i + 1 == ranges.size())
                    continue;
                transport.begin([&, i, pass, sample](MetricResult &result) {
                    if (fault == "consumer")
                        throw std::runtime_error("consumer");
                    Json values = Json::array();
                    for (const auto &metric : selected.at("metrics")) {
                        const auto type = metric.at("result_type").get<unsigned>();
                        const auto n = result.raw.at(0);
                        values.push_back({{"type", type},
                                          {"value", type == 3   ? Json(n % 2 == 0)
                                                    : type == 2 ? Json(double(n) + 0.25)
                                                                : Json(1000u + n)}});
                    }
                    transport.deliver(selected, {{"set", selected["name"]},
                                                 {"event", ranges[i]},
                                                 {"pass_index", pass},
                                                 {"sample_index", sample},
                                                 {"raw_report", "fixture.bin"},
                                                 {"raw_sha256", std::string(64, '0')},
                                                 {"available", job.value("all_available", false) ||
                                                                   result.values["available"].get<bool>()},
                                                 {"unavailable_reasons", Json::array()},
                                                 {"values", values},
                                                 {"information", Json::array()}});
                });
                if (fault == "acquire")
                    throw std::runtime_error("acquire");
                transport.submit();
            }
        },
        [&](const Json &) {
            auto result = std::make_unique<SessionLock>(samples, deviceClosed);
            lock = result.get();
            return result;
        });
    Json output{{"catalog", session.catalog()}, {"plan", session.plan()}, {"ids", session.requestedIds()}};
    for (const auto &step : job.at("steps")) {
        Json row;
        try {
            const auto op = step.at("op");
            if (op == "prepare")
                row["return"] = session.prepare(step.at("ids"));
            else if (op == "begin")
                session.begin({});
            else if (op == "submit")
                session.submit();
            else if (op == "catalog")
                row["return"] = session.descriptions();
            else if (op == "replay")
                row["return"] =
                    session.replay(step.value("pass", 0u), step.at("ranges"), step.value("flag", false));
            else if (op == "collect")
                row["return"] = MetricIterationRunner(session).execute(
                    session.requestedIds(), step.at("ranges"), step.value("options", Json::object()));
            else if (op == "fault") {
                samples.error = step.value("driver", "");
                fault = step.value("acquire", "");
            } else if (op == "close")
                session.close();
            if (!row.contains("return"))
                row["return"] = nullptr;
        } catch (const std::exception &e) {
            row["error"] = e.what();
        }
        row["closed"] = session.closed();
        try {
            row["audit"] = session.audit();
        } catch (const std::exception &e) {
            row["audit_error"] = e.what();
        }
        row["publisher"] = session.publisherValues().report();
        row["trace"] = samples.trace;
        samples.trace.clear();
        steps.push_back(row);
    }
    // All fault scenarios have an explicit final retry, so no leaked test lock is allowed.
    samples.error.clear();
    closeFailures = 0;
    session.close();
    output["steps"] = steps;
    return output;
}
