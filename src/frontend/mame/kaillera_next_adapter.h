// license:BSD-3-Clause

#ifndef MAME_FRONTEND_MAME_KAILLERA_NEXT_ADAPTER_H
#define MAME_FRONTEND_MAME_KAILLERA_NEXT_ADAPTER_H

#pragma once

#include "emu.h"

#include <memory>

class kaillera_next_adapter
{
public:
	static std::unique_ptr<kaillera_next_adapter> create(running_machine &machine);

	kaillera_next_adapter(running_machine &machine);
	~kaillera_next_adapter();

	bool initialize();
	bool tick();
	void frame_done();
	void on_exit();

public:
	// Exposed so C ABI callback shims in the .cpp can cast the opaque user
	// pointer without making adapter internals part of MAME's public API.
	struct impl;

private:
	std::unique_ptr<impl> m_impl;
};

#endif // MAME_FRONTEND_MAME_KAILLERA_NEXT_ADAPTER_H
