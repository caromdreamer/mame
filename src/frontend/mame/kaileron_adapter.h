// license:BSD-3-Clause

#ifndef MAME_FRONTEND_MAME_KAILERON_ADAPTER_H
#define MAME_FRONTEND_MAME_KAILERON_ADAPTER_H

#pragma once

#include "emu.h"

#include <memory>

enum class kaileron_frame_mode;

class kaileron_adapter
{
public:
	static std::unique_ptr<kaileron_adapter> create(running_machine &machine);

	kaileron_adapter(running_machine &machine);
	~kaileron_adapter();

	bool initialize();
	bool tick();
	void frame_done(kaileron_frame_mode mode);
	void frame_notifier();
	void on_exit();
#if defined(__EMSCRIPTEN__)
	const char *web_metrics();
	u32 web_playback_speed_percent() const;
	int web_correct_input(u32 frame, u32 player, u32 byte, u32 xor_mask);
	int web_dump_snapshot(u32 frame);
	int web_snapshot_manifest(u32 frame);
	int web_set_runahead(u32 frames);
#endif

public:
	// Exposed so C ABI callback shims in the .cpp can cast the opaque user
	// pointer without making adapter internals part of MAME's public API.
	struct impl;

private:
	std::unique_ptr<impl> m_impl;
};

#endif // MAME_FRONTEND_MAME_KAILERON_ADAPTER_H
