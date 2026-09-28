/*
 * Copyright 2010-2016 OpenXcom Developers.
 *
 * This file is part of OpenXcom.
 *
 * OpenXcom is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * OpenXcom is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with OpenXcom.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "Window.h"
#include <SDL.h>
#include <SDL_mixer.h>
#include "../fmath.h"
#include "../Engine/Timer.h"
#include "../Engine/Sound.h"
#include "../Engine/RNG.h"
#include "../Engine/Exception.h"
#include "../Engine/HdUiBadge.h"

namespace OpenXcom
{

const double Window::POPUP_SPEED = 0.05;

Sound *Window::soundPopup[3];

/**
 * Sets up a blank window with the specified size and position.
 * @param state Pointer to state the window belongs to.
 * @param width Width in pixels.
 * @param height Height in pixels.
 * @param x X position in pixels.
 * @param y Y position in pixels.
 * @param popup Popup animation.
 */
Window::Window(State *state, int width, int height, int x, int y, WindowPopup popup) : Surface(width, height, x, y),
	_dx(-x), _dy(-y), _bg(0), _color(0), _popup(popup), _popupStep(0.0), _state(state), _contrast(false), _screen(false), _thinBorder(false), _innerColor(0), _mute(false)
{
	_timer = new Timer(10);
	_timer->onTimer((SurfaceHandler)&Window::popup);

	if (_popup == POPUP_NONE)
	{
		_popupStep = 1.0;
	}
	else
	{
		setHidden(true);
		_timer->start();
		if (_state != 0)
		{
			_screen = state->isScreen();
			if (_screen)
				_state->toggleScreen();
		}
	}
}

/**
 * Deletes timers.
 */
Window::~Window()
{
	delete _timer;
}

/**
 * Changes the surface used to draw the background of the window.
 * @param bg New background.
 */
void Window::setBackground(const Surface *bg, const std::string &imageId)
{
	_bg = bg;
	_hdBackgroundId = !imageId.empty() ? imageId : (bg ? bg->getHdResourceId() : std::string{});
	_hdBackground = HdUiImageDefinition{};
	_redraw = true;
}

/**
 * Changes the color used to draw the shaded border.
 * @param color Color value.
 */
void Window::setColor(Uint8 color)
{
	_color = color;
	_redraw = true;
}

/**
 * Returns the color used to draw the shaded border.
 * @return Color value.
 */
Uint8 Window::getColor() const
{
	return _color;
}

/**
 * Enables/disables high contrast color. Mostly used for
 * Battlescape UI.
 * @param contrast High contrast setting.
 */
void Window::setHighContrast(bool contrast)
{
	_contrast = contrast;
	_redraw = true;
}

/**
 * Keeps the animation timers running.
 */
void Window::think()
{
	if (_hidden && _popupStep < 1.0)
	{
		_state->hideAll();
		setHidden(false);
	}

	_timer->think(0, this);
}

/**
 * Plays the window popup animation.
 */
void Window::popup()
{
	if (!_mute && AreSame(_popupStep, 0.0))
	{
		int sound = RNG::seedless(0,2);
		if (soundPopup[sound] != 0)
		{
			soundPopup[sound]->play(Mix_GroupAvailable(0));
		}
	}
	if (_popupStep < 1.0)
	{
		_popupStep += POPUP_SPEED;
	}
	else
	{
		if (_screen)
		{
			_state->toggleScreen();
		}
		_state->showAll();
		_popupStep = 1.0;
		_timer->stop();
	}
	_redraw = true;
}

/**
 * Draws the bordered window with a graphic background.
 * The background never moves with the window, it's
 * always aligned to the top-left corner of the screen
 * and cropped to fit the inside area.
 */
void Window::composeHd(HdCanvas &canvas, HdImageCache &images)
{
	if (!isDisplayVisible()) return;
	HdCanvas window(getWidth(), getHeight());
	composeHdContents(window, images, hdUiIdentityPalette());
	if (_popupStep >= 1.0) hdAppendPipelineBadge(window);
	composeHdLayer(canvas, window);
}

void Window::composeHdBackdrop(HdCanvas &canvas, HdImageCache &images,
	HdRect source, HdRect destination, const HdUiIndexMap &paletteMap) const
{
	if (!source.finite() || !destination.finite() || source.w < 0 || source.h < 0 ||
		destination.w < 0 || destination.h < 0)
		throw Exception("[HD UI ERROR] Invalid window backdrop crop");
	if (source.empty() || destination.empty()) return;
	HdCanvas window(getWidth(), getHeight());
	composeHdContents(window, images, paletteMap);
	HdCanvas region(source.w, source.h);
	region.composite(window, {-source.x, -source.y, 1, 1});
	canvas.composite(region, {destination.x, destination.y, destination.w / source.w, destination.h / source.h});
}

void Window::composeHdContents(HdCanvas &window, HdImageCache &images, const HdUiIndexMap &paletteMap) const
{
	const bool horizontal = _popup == POPUP_HORIZONTAL || _popup == POPUP_BOTH;
	const bool vertical = _popup == POPUP_VERTICAL || _popup == POPUP_BOTH;
	const double step = std::max(0.0, std::min(1.0, _popupStep));
	HdRect square{
		double(horizontal ? int((getWidth() - getWidth() * step) / 2) : 0),
		double(vertical ? int((getHeight() - getHeight() * step) / 2) : 0),
		double(horizontal ? int(getWidth() * step) : getWidth()),
		double(vertical ? int(getHeight() * step) : getHeight())};
	if (square.empty()) return;
	const int mul = _contrast ? 2 : 1;
	const auto rect = [&](HdRect bounds, int color)
	{
		bounds.w = std::max(0.0, bounds.w); bounds.h = std::max(0.0, bounds.h);
		window.sourceRectangle(bounds, getHdColor(paletteMap[static_cast<Uint8>(color)]));
	};
	int color = _color + (_thinBorder ? 1 : 3) * mul;
	for (int i = 0; i < 5; ++i)
	{
		rect(square, color);
		if (_thinBorder)
		{
			if (i % 2 == 0) { ++square.x; ++square.y; }
			square.w = std::max(0.0, square.w - 1); square.h = std::max(0.0, square.h - 1);
			switch (i)
			{
			case 0: color = _color + 5 * mul; rect({square.w, 0, 1, 1}, color); break;
			case 1: color = _color + 2 * mul; break;
			case 2: color = _color + 4 * mul; rect({square.w + 1, 1, 1, 1}, color); break;
			case 3: color = _color + 3 * mul; break;
			default: break;
			}
		}
		else
		{
			color += (i < 2 ? -1 : 1) * mul;
			++square.x; ++square.y;
			square.w = square.w >= 2 ? square.w - 2 : 1;
			square.h = square.h >= 2 ? square.h - 2 : 1;
		}
	}
	if (!_thinBorder && _innerColor) rect(square, _innerColor);
	if (_bg)
	{
		if (_hdBackground.key.family.empty() && !_hdBackgroundId.empty())
			_hdBackground = hdUiScreenDefinition(_hdBackgroundId, _bg->getWidth(), _bg->getHeight(), _bg->getHdUiManaLayout());
		if (_hdBackground.key.family.empty())
			throw Exception("[HD UI ERROR] Window background has no semantic HD resource identity");
		auto palette = std::make_shared<std::array<HdRgba, 256>>();
		for (size_t i = 0; i < palette->size(); ++i) (*palette)[i] = getHdColor(paletteMap[i]);
		hdAppendUiImage(window, images, _hdBackground,
			{square.x - _dx, square.y - _dy, square.w, square.h}, square, palette);
	}
}

void Window::draw()
{
	Surface::draw();
	SDL_Rect square;

	if (_popup == POPUP_HORIZONTAL || _popup == POPUP_BOTH)
	{
		square.x = (int)((getWidth() - getWidth() * _popupStep) / 2);
		square.w = (int)(getWidth() * _popupStep);
	}
	else
	{
		square.x = 0;
		square.w = getWidth();
	}
	if (_popup == POPUP_VERTICAL || _popup == POPUP_BOTH)
	{
		square.y = (int)((getHeight() - getHeight() * _popupStep) / 2);
		square.h = (int)(getHeight() * _popupStep);
	}
	else
	{
		square.y = 0;
		square.h = getHeight();
	}

	int mul = 1;
	if (_contrast)
	{
		mul = 2;
	}
	Uint8 color = _color + 3 * mul;

	if (_thinBorder)
	{
		color = _color + 1 * mul;
		for (int i = 0; i < 5; ++i)
		{
			drawRect(&square, color);

			if (i % 2 == 0)
			{
				square.x++;
				square.y++;
			}
			square.w--;
			square.h--;

			switch (i)
			{
			case 0:
				color = _color + 5 * mul;
				setPixel(square.w, 0, color);
				break;
			case 1:
				color = _color + 2 * mul;
				break;
			case 2:
				color = _color + 4 * mul;
				setPixel(square.w+1, 1, color);
				break;
			case 3:
				color = _color + 3 * mul;
				break;
			}
		}
	}
	else
	{
		for (int i = 0; i < 5; ++i)
		{
			drawRect(&square, color);
			if (i < 2)
				color -= 1 * mul;
			else
				color += 1 * mul;
			square.x++;
			square.y++;
			if (square.w >= 2)
				square.w -= 2;
			else
				square.w = 1;

			if (square.h >= 2)
				square.h -= 2;
			else
				square.h = 1;
		}
		if (_innerColor != 0)
		{
			drawRect(&square, _innerColor);
		}
	}

	if (_bg != 0)
	{
		SurfaceCrop crop = _bg->getCrop();
		crop.getCrop()->x = square.x - _dx;
		crop.getCrop()->y = square.y - _dy;
		crop.getCrop()->w = square.w ;
		crop.getCrop()->h = square.h ;
		crop.setX(square.x);
		crop.setY(square.y);
		crop.blit(this);
	}
}

/**
 * Changes the horizontal offset of the surface in the X axis.
 * @param dx X position in pixels.
 */
void Window::setDX(int dx)
{
	_dx = dx;
}

/**
 * Changes the vertical offset of the surface in the Y axis.
 * @param dy Y position in pixels.
 */
void Window::setDY(int dy)
{
	_dy = dy;
}

/**
 * Changes the window to have a thin border.
 */
void Window::setThinBorder()
{
	_thinBorder = true;
}

/**
 * Changes the window to have a custom inner color.
 */
void Window::setInnerColor(Uint8 innerColor)
{
	_innerColor = innerColor;
}

}
