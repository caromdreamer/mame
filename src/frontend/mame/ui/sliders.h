// license:BSD-3-Clause
// copyright-holders:Nicola Salmoria, Aaron Giles, Nathan Woods
/***************************************************************************

    ui/sliders.h

    Internal MAME menus for the user interface.

***************************************************************************/

#ifndef MAME_FRONTEND_UI_SLIDERS_H
#define MAME_FRONTEND_UI_SLIDERS_H

#pragma once

#include "ui/menu.h"


namespace ui {

class menu_sliders : public menu
{
public:
	menu_sliders(mame_ui_manager &mui, render_target &target, bool menuless_mode = false);
	virtual ~menu_sliders() override;
#if defined(__EMSCRIPTEN__)
	static int browser_selection_index(mame_ui_manager &mui)
	{
		menu_sliders *slider = topmost_menu<menu_sliders>(mui);
		return slider && slider->selection_valid() ? slider->selected_index() : -1;
	}
	static bool browser_gamma_selected(mame_ui_manager &mui)
	{
		menu_sliders *slider = topmost_menu<menu_sliders>(mui);
		if (!slider || !slider->selection_valid()) return false;
		menu_item const &item = slider->selected_item();
		return item.type() == menu_item_type::SLIDER && item.text().size() >= 6 &&
				item.text().compare(item.text().size() - 6, 6, " Gamma") == 0;
	}
#endif

protected:
	virtual void recompute_metrics(uint32_t width, uint32_t height, float aspect) override;
	virtual void custom_render(uint32_t flags, void *selectedref, float top, float bottom, float origx1, float origy1, float origx2, float origy2) override;
	virtual void menu_activated() override;
	virtual void menu_deactivated() override;

private:
	virtual void populate() override;
	virtual bool handle(event const *ev) override;

	bool const m_menuless_mode;
	bool m_hidden;
};

} // namespace ui

#endif // MAME_FRONTEND_UI_SLIDERS_H
