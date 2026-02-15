/*
 * Helix - Decentralized relay node for encrypted messaging
 *
 * Copyright (c) 2026 DNA Messenger Team
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "helix/node.h"
#include "helix/config.h"
#include "helix/log.h"

#include <csignal>
#include <cstdlib>
#include <iostream>

static std::atomic<bool> g_running{true};

static void signal_handler(int sig)
{
    (void)sig;
    g_running = false;
}

int main(int argc, char *argv[])
{
    helix::Config config;
    if (!config.parse(argc, argv)) {
        return EXIT_FAILURE;
    }

    helix::log::init(config.log_level);
    auto logger = helix::log::get("main");

    logger->info("Helix v{} starting", HELIX_VERSION);

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    helix::Node node(config);

    if (!node.start()) {
        logger->error("Failed to start node");
        return EXIT_FAILURE;
    }

    logger->info("Helix running on {}:{}", config.listen_addr, config.listen_port);

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    logger->info("Shutting down");
    node.stop();

    return EXIT_SUCCESS;
}
