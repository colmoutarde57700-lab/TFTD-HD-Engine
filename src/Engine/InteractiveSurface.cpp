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
#include "InteractiveSurface.h"
#include "Action.h"
#include "Logger.h"
#include "Options.h"
#include "PresentationSpaces.h"
#include <typeinfo>

namespace OpenXcom
{

const SDLKey InteractiveSurface::SDLK_ANY = (SDLKey)-1; // using an unused keycode to represent an "any key"

/**
 * Sets up a blank interactive surface with the specified size and position.
 * @param width Width in pixels.
 * @param height Height in pixels.
 * @param x X position in pixels.
 * @param y Y position in pixels.
 */
InteractiveSurface::InteractiveSurface(int width, int height, int x, int y) : Surface(width, height, x, y), _buttonsPressed(0), _presentationInputEnabled(false), _presentationInputX(0), _presentationInputY(0), _presentationInputW(0), _presentationInputH(0), _presentationInputPhysicalX(0), _presentationInputPhysicalY(0), _presentationInputScaleX(1.0), _presentationInputScaleY(1.0), _in(0), _over(0), _out(0), _isHovered(false), _isFocused(true), _listButton(false), _tftdMode(false)
{
}

/**
 *
 */
InteractiveSurface::~InteractiveSurface()
{
}

void InteractiveSurface::setPresentationInputTransform(int logicalX, int logicalY, int logicalW, int logicalH,
	int physicalX, int physicalY, double scaleX, double scaleY)
{
	_presentationInputEnabled = logicalW > 0 && logicalH > 0 && scaleX > 0.0 && scaleY > 0.0;
	_presentationInputX = logicalX;
	_presentationInputY = logicalY;
	_presentationInputW = logicalW;
	_presentationInputH = logicalH;
	_presentationInputPhysicalX = physicalX;
	_presentationInputPhysicalY = physicalY;
	_presentationInputScaleX = scaleX > 0.0 ? scaleX : 1.0;
	_presentationInputScaleY = scaleY > 0.0 ? scaleY : 1.0;
}

void InteractiveSurface::clearPresentationInputTransform()
{
	_presentationInputEnabled = false;
}

bool InteractiveSurface::isButtonHandled(Uint8 button)
{
	bool handled = (_click.find(0) != _click.end() ||
					_press.find(0) != _press.end() ||
					_release.find(0) != _release.end());
	if (!handled && button != 0)
	{
		handled = (_click.find(button) != _click.end() ||
				   _press.find(button) != _press.end() ||
				   _release.find(button) != _release.end());
	}
	return handled;
}

bool InteractiveSurface::isButtonPressed(Uint8 button) const
{
	if (button == 0)
	{
		return (_buttonsPressed != 0);
	}
	else
	{
		return (_buttonsPressed & SDL_BUTTON(button)) != 0;
	}
}

void InteractiveSurface::setButtonPressed(Uint8 button, bool pressed)
{
	if (pressed)
	{
		_buttonsPressed |= SDL_BUTTON(button);
	}
	else
	{
		_buttonsPressed &= (~SDL_BUTTON(button));
	}
}

/**
 * Changes the visibility of the surface. A hidden surface
 * isn't blitted nor receives events.
 * @param visible New visibility.
 */
void InteractiveSurface::setVisible(bool visible)
{
	Surface::setVisible(visible);
	// Unpress button if it was hidden
	if (!_visible)
	{
		unpress(0);
	}
}

/**
 * Called whenever an action occurs, and processes it to
 * check if it's relevant to the surface and convert it
 * into a meaningful interaction like a "click", calling
 * the respective handlers.
 * @param action Pointer to an action.
 * @param state State that the action handlers belong to.
 */
void InteractiveSurface::handle(Action *action, State *state)
{
	if (!_visible || _hidden)
		return;

	action->setSender(this);

	const int displayX = _displayAnchorX + (getX() - _displayAnchorX) * _displayScale;
	const int displayY = _displayAnchorY + (getY() - _displayAnchorY) * _displayScale;
	const int displayRight = _displayAnchorX + ((getX() - _displayAnchorX) + getWidth()) * _displayScale;
	const int displayBottom = _displayAnchorY + ((getY() - _displayAnchorY) + getHeight()) * _displayScale;
	double hitX = -1.0;
	double hitY = -1.0;
	int hitLeft = displayX;
	int hitTop = displayY;
	int hitRight = displayRight;
	int hitBottom = displayBottom;

	if (action->getDetails()->type == SDL_MOUSEBUTTONUP || action->getDetails()->type == SDL_MOUSEBUTTONDOWN)
	{
		action->setMouseAction(action->getDetails()->button.x, action->getDetails()->button.y, getX(), getY(), _displayScale, _displayScale);
		action->setPresentationTransform(_displayAnchorX, _displayAnchorY, _displayScale, _displayScale);
	}
	else if (action->getDetails()->type == SDL_MOUSEMOTION)
	{
		action->setMouseAction(action->getDetails()->motion.x, action->getDetails()->motion.y, getX(), getY(), _displayScale, _displayScale);
		action->setPresentationTransform(_displayAnchorX, _displayAnchorY, _displayScale, _displayScale);
	}

	if (action->isMouseAction())
	{
		if (_presentationInputEnabled)
		{
			int physicalX = -1;
			int physicalY = -1;
			if (action->getDetails()->type == SDL_MOUSEBUTTONUP || action->getDetails()->type == SDL_MOUSEBUTTONDOWN)
			{
				physicalX = action->getDetails()->button.x;
				physicalY = action->getDetails()->button.y;
			}
			else if (action->getDetails()->type == SDL_MOUSEMOTION)
			{
				physicalX = action->getDetails()->motion.x;
				physicalY = action->getDetails()->motion.y;
			}
			if (physicalX >= 0 && physicalY >= 0)
			{
				hitX = (physicalX - _presentationInputPhysicalX) / _presentationInputScaleX;
				hitY = (physicalY - _presentationInputPhysicalY) / _presentationInputScaleY;
					// BATTLE_UI_FAMILY_V1-B: once a Surface belongs to an explicit
					// presentation space, Action's absolute/relative mouse queries must
					// describe that SAME logical space, not the current World canvas.
					// This preserves Slider/ComboBox/TextList legacy math while divorcing
					// the UI from battlescapeScale.
					action->setLogicalMouseOverride(hitX, hitY);
				hitLeft = _presentationInputX;
				hitTop = _presentationInputY;
				hitRight = _presentationInputX + _presentationInputW;
				hitBottom = _presentationInputY + _presentationInputH;
			}
		}
		else
		{
			hitX = action->getDisplayXMouse();
			hitY = action->getDisplayYMouse();
		}

		// Hit-testing happens in DISPLAY logical coordinates. Once inside,
		// handlers see getAbsolute*Mouse() inverse-transformed back into the
		// original 320x200-era coordinate system, so legacy UI math continues
		// to work without per-screen hacks.
		if ((hitX >= hitLeft && hitX < hitRight) &&
			(hitY >= hitTop && hitY < hitBottom))
		{
			if (!_isHovered)
			{
				_isHovered = true;
				mouseIn(action, state);
			}
			if (_listButton && action->getDetails()->type == SDL_MOUSEMOTION)
			{
				_buttonsPressed = SDL_GetMouseState(0, 0);
				for (Uint8 i = 1; i <= NUM_BUTTONS; ++i)
				{
					if (isButtonPressed(i))
					{
						action->getDetails()->button.button = i;
						mousePress(action, state);
					}
				}
			}
			mouseOver(action, state);
		}
		else
		{
			if (_isHovered)
			{
				_isHovered = false;
				mouseOut(action, state);
				if (_listButton && action->getDetails()->type == SDL_MOUSEMOTION)
				{
					for (Uint8 i = 1; i <= NUM_BUTTONS; ++i)
					{
						if (isButtonPressed(i))
						{
							setButtonPressed(i, false);
						}
						action->getDetails()->button.button = i;
						mouseRelease(action, state);
					}
				}
			}
		}
	}

	if (action->getDetails()->type == SDL_MOUSEBUTTONDOWN)
	{
		if (_isHovered && isButtonHandled(action->getDetails()->button.button))
		{
			Log(LOG_INFO) << "[PRESENTATION-SPACES TRACE V1][INPUT] state="
				<< (state ? typeid(*state).name() : "<null>")
				<< " surface=" << typeid(*this).name()
				<< " physical=" << action->getDetails()->button.x << "," << action->getDetails()->button.y
				<< " displayLogical=" << action->getDisplayXMouse() << "," << action->getDisplayYMouse()
				<< " originalLogical=" << action->getAbsoluteXMouse() << "," << action->getAbsoluteYMouse()
				<< " screenScale=" << action->getXScale() << "x" << action->getYScale()
				<< " bands(LT)=" << action->getLeftBlackBand() << "," << action->getTopBlackBand()
				<< " surfaceLogical=" << getX() << "," << getY() << "," << getWidth() << "x" << getHeight()
				<< " surfaceDisplay=" << displayX << "," << displayY << "," << (displayRight-displayX) << "x" << (displayBottom-displayY)
				<< " displayScale=" << _displayScale
				<< " anchor=" << _displayAnchorX << "," << _displayAnchorY;
			if (_presentationInputEnabled)
			{
				Log(LOG_INFO) << "[BATTLE-UI INPUT CUTOVER V1] state="
					<< (state ? typeid(*state).name() : "<null>")
					<< " surface=" << typeid(*this).name()
					<< " physical=" << action->getDetails()->button.x << "," << action->getDetails()->button.y
					<< " uiLogical=" << hitX << "," << hitY
					<< " uiRect=" << _presentationInputX << "," << _presentationInputY << ","
					<< _presentationInputW << "x" << _presentationInputH
					<< " uiScale=" << _presentationInputScaleX << "x" << _presentationInputScaleY
						<< " physicalOrigin=" << _presentationInputPhysicalX << "," << _presentationInputPhysicalY
						<< " actionAbs=" << action->getAbsoluteXMouse() << "," << action->getAbsoluteYMouse()
						<< " actionRel=" << action->getRelativeXMouse() << "," << action->getRelativeYMouse();
			}
			// CONTRACT V1 SHADOW R2: calculate the candidate UI-space inverse
			// independently of the historical world/base canvas. This is diagnostic
			// only; event dispatch and hit-testing still use the legacy Action values.
			const PresentationTransform shadowUi = PresentationSpacesContract::uniformUiFit(
				640, 360, Options::displayWidth, Options::displayHeight);
			const double shadowUiX = shadowUi.physicalToLogicalX(action->getDetails()->button.x);
			const double shadowUiY = shadowUi.physicalToLogicalY(action->getDetails()->button.y);
			const bool shadowUiInside =
				action->getDetails()->button.x >= shadowUi.physicalContent.x &&
				action->getDetails()->button.x < shadowUi.physicalContent.x + shadowUi.physicalContent.w &&
				action->getDetails()->button.y >= shadowUi.physicalContent.y &&
				action->getDetails()->button.y < shadowUi.physicalContent.y + shadowUi.physicalContent.h;
			Log(LOG_INFO) << "[PRESENTATION-SPACES CONTRACT V1 R2][INPUT-SHADOW] state="
				<< (state ? typeid(*state).name() : "<null>")
				<< " surface=" << typeid(*this).name()
				<< " physical=" << action->getDetails()->button.x << "," << action->getDetails()->button.y
				<< " legacyDisplayLogical=" << action->getDisplayXMouse() << "," << action->getDisplayYMouse()
				<< " uiLogicalCandidate=" << shadowUiX << "," << shadowUiY
				<< " uiScale=" << shadowUi.scaleX << "x" << shadowUi.scaleY
				<< " uiContent=" << shadowUi.physicalContent.x << "," << shadowUi.physicalContent.y << ","
				<< shadowUi.physicalContent.w << "x" << shadowUi.physicalContent.h
				<< " inside=" << (shadowUiInside ? 1 : 0);
		}
		if (_isHovered && !isButtonPressed(action->getDetails()->button.button))
		{
			setButtonPressed(action->getDetails()->button.button, true);
			mousePress(action, state);
		}
	}
	else if (action->getDetails()->type == SDL_MOUSEBUTTONUP)
	{
		if (isButtonPressed(action->getDetails()->button.button))
		{
			setButtonPressed(action->getDetails()->button.button, false);
			mouseRelease(action, state);
			if (_isHovered)
			{
				mouseClick(action, state);
			}
		}
	}

	if (_isFocused)
	{
		if (action->getDetails()->type == SDL_KEYDOWN)
		{
			keyboardPress(action, state);
		}
		else if (action->getDetails()->type == SDL_KEYUP)
		{
			keyboardRelease(action, state);
		}
	}
}

/**
 * Changes the surface's focus. Surfaces will only receive
 * keyboard events if focused.
 * @param focus Is it focused?
 */
void InteractiveSurface::setFocus(bool focus, bool modal)
{
	_isFocused = focus;
}

/**
 * Returns the surface's focus. Surfaces will only receive
 * keyboard events if focused.
 * @return Is it focused?
 */
bool InteractiveSurface::isFocused() const
{
	return _isFocused;
}

/**
 * Simulates a "mouse button release". Used in circumstances
 * where the surface is unpressed without user input.
 * @param state Pointer to running state.
 */
void InteractiveSurface::unpress(State *state)
{
	if (isButtonPressed())
	{
		_buttonsPressed = 0;
		SDL_Event ev;
		ev.type = SDL_MOUSEBUTTONUP;
		ev.button.button = SDL_BUTTON_LEFT;
		Action a = Action(&ev, 0.0, 0.0, 0, 0);
		mouseRelease(&a, state);
	}
}

/**
 * Called every time there's a mouse press over the surface.
 * Allows the surface to have custom functionality for this action,
 * and can be called externally to simulate the action.
 * @param action Pointer to an action.
 * @param state State that the action handlers belong to.
 */
void InteractiveSurface::mousePress(Action *action, State *state)
{
	auto allHandler = _press.find(0);
	auto oneHandler = _press.find(action->getDetails()->button.button);
	if (allHandler != _press.end())
	{
		ActionHandler handler = allHandler->second;
		(state->*handler)(action);
	}
	if (oneHandler != _press.end())
	{
		ActionHandler handler = oneHandler->second;
		(state->*handler)(action);
	}
}

/**
 * Called every time there's a mouse release over the surface.
 * Allows the surface to have custom functionality for this action,
 * and can be called externally to simulate the action.
 * @param action Pointer to an action.
 * @param state State that the action handlers belong to.
 */
void InteractiveSurface::mouseRelease(Action *action, State *state)
{
	auto allHandler = _release.find(0);
	auto oneHandler = _release.find(action->getDetails()->button.button);
	if (allHandler != _release.end())
	{
		ActionHandler handler = allHandler->second;
		(state->*handler)(action);
	}
	if (oneHandler != _release.end())
	{
		ActionHandler handler = oneHandler->second;
		(state->*handler)(action);
	}
}

/**
 * Called every time there's a mouse click on the surface.
 * Allows the surface to have custom functionality for this action,
 * and can be called externally to simulate the action.
 * @param action Pointer to an action.
 * @param state State that the action handlers belong to.
 */
void InteractiveSurface::mouseClick(Action *action, State *state)
{
	auto allHandler = _click.find(0);
	auto oneHandler = _click.find(action->getDetails()->button.button);
	if (allHandler != _click.end())
	{
		ActionHandler handler = allHandler->second;
		(state->*handler)(action);
	}
	if (oneHandler != _click.end())
	{
		ActionHandler handler = oneHandler->second;
		(state->*handler)(action);
	}
}

/**
 * Called every time the mouse moves into the surface.
 * Allows the surface to have custom functionality for this action,
 * and can be called externally to simulate the action.
 * @param action Pointer to an action.
 * @param state State that the action handlers belong to.
 */
void InteractiveSurface::mouseIn(Action *action, State *state)
{
	if (_in != 0)
	{
		(state->*_in)(action);
	}
}

/**
 * Called every time the mouse moves over the surface.
 * Allows the surface to have custom functionality for this action,
 * and can be called externally to simulate the action.
 * @param action Pointer to an action.
 * @param state State that the action handlers belong to.
 */
void InteractiveSurface::mouseOver(Action *action, State *state)
{
	if (_over != 0)
	{
		(state->*_over)(action);
	}
}

/**
 * Called every time the mouse moves out of the surface.
 * Allows the surface to have custom functionality for this action,
 * and can be called externally to simulate the action.
 * @param action Pointer to an action.
 * @param state State that the action handlers belong to.
 */
void InteractiveSurface::mouseOut(Action *action, State *state)
{
	if (_out != 0)
	{
		(state->*_out)(action);
	}
}

/**
 * Called every time there's a keyboard press when the surface is focused.
 * Allows the surface to have custom functionality for this action,
 * and can be called externally to simulate the action.
 * @param action Pointer to an action.
 * @param state State that the action handlers belong to.
 */
void InteractiveSurface::keyboardPress(Action *action, State *state)
{
	auto allHandler = _keyPress.find(SDLK_ANY);
	auto oneHandler = _keyPress.find(action->getDetails()->key.keysym.sym);
	if (allHandler != _keyPress.end())
	{
		ActionHandler handler = allHandler->second;
		(state->*handler)(action);
	}
	// Check if Ctrl, Alt and Shift aren't pressed
	bool mod = ((action->getDetails()->key.keysym.mod & (KMOD_CTRL|KMOD_ALT|KMOD_SHIFT)) != 0);
	if (oneHandler != _keyPress.end() && !mod)
	{
		ActionHandler handler = oneHandler->second;
		(state->*handler)(action);
	}
}

/**
 * Called every time there's a keyboard release over the surface.
 * Allows the surface to have custom functionality for this action,
 * and can be called externally to simulate the action.
 * @param action Pointer to an action.
 * @param state State that the action handlers belong to.
 */
void InteractiveSurface::keyboardRelease(Action *action, State *state)
{
	auto allHandler = _keyRelease.find(SDLK_ANY);
	auto oneHandler = _keyRelease.find(action->getDetails()->key.keysym.sym);
	if (allHandler != _keyRelease.end())
	{
		ActionHandler handler = allHandler->second;
		(state->*handler)(action);
	}
	// Check if Ctrl, Alt and Shift aren't pressed
	bool mod = ((action->getDetails()->key.keysym.mod & (KMOD_CTRL|KMOD_ALT|KMOD_SHIFT)) != 0);
	if (oneHandler != _keyRelease.end() && !mod)
	{
		ActionHandler handler = oneHandler->second;
		(state->*handler)(action);
	}
}

/**
 * Sets a function to be called every time the surface is mouse clicked.
 * @param handler Action handler.
 * @param button Mouse button to check for. Set to 0 for any button.
 */
void InteractiveSurface::onMouseClick(ActionHandler handler, Uint8 button)
{
	if (handler != 0)
	{
		_click[button] = handler;
	}
	else
	{
		_click.erase(button);
	}
}

/**
 * Sets a function to be called every time the surface is mouse pressed.
 * @param handler Action handler.
 * @param button Mouse button to check for. Set to 0 for any button.
 */
void InteractiveSurface::onMousePress(ActionHandler handler, Uint8 button)
{
	if (handler != 0)
	{
		_press[button] = handler;
	}
	else
	{
		_press.erase(button);
	}
}

/**
 * Sets a function to be called every time the surface is mouse released.
 * @param handler Action handler.
 * @param button Mouse button to check for. Set to 0 for any button.
 */
void InteractiveSurface::onMouseRelease(ActionHandler handler, Uint8 button)
{
	if (handler != 0)
	{
		_release[button] = handler;
	}
	else
	{
		_release.erase(button);
	}
}

/**
 * Sets a function to be called every time the mouse moves into the surface.
 * @param handler Action handler.
 */
void InteractiveSurface::onMouseIn(ActionHandler handler)
{
	_in = handler;
}

/**
 * Sets a function to be called every time the mouse moves over the surface.
 * @param handler Action handler.
 */
void InteractiveSurface::onMouseOver(ActionHandler handler)
{
	_over = handler;
}

/**
 * Sets a function to be called every time the mouse moves out of the surface.
 * @param handler Action handler.
 */
void InteractiveSurface::onMouseOut(ActionHandler handler)
{
	_out = handler;
}

/**
 * Sets a function to be called every time a key is pressed when the surface is focused.
 * @param handler Action handler.
 * @param key Keyboard button to check for (note: ignores key modifiers). Set to SDLK_ANY for any key.
 */
void InteractiveSurface::onKeyboardPress(ActionHandler handler, SDLKey key)
{
	if (key == SDLK_UNKNOWN)
	{
		// Ignore unknown keys
		return;
	}
	if (handler != 0)
	{
		_keyPress[key] = handler;
	}
	else
	{
		_keyPress.erase(key);
	}
}

/**
 * Sets a function to be called every time a key is released when the surface is focused.
 * @param handler Action handler.
 * @param key Keyboard button to check for (note: ignores key modifiers). Set to SDLK_ANY for any key.
 */
void InteractiveSurface::onKeyboardRelease(ActionHandler handler, SDLKey key)
{
	if (key == SDLK_UNKNOWN)
	{
		// Ignore unknown keys
		return;
	}
	if (handler != 0)
	{
		_keyRelease[key] = handler;
	}
	else
	{
		_keyRelease.erase(key);
	}
}

/**
 * Sets a flag for this button to say "i'm a member of a textList" to true.
 */
void InteractiveSurface::setListButton()
{
	_listButton = true;
}

/**
 * Returns the help description of this surface,
 * for example for showing in tooltips.
 * @return String ID.
 */
std::string InteractiveSurface::getTooltip() const
{
	return _tooltip;
}

/**
 * Changes the help description of this surface,
 * for example for showing in tooltips.
 * @param tooltip String ID.
 */
void InteractiveSurface::setTooltip(const std::string &tooltip)
{
	_tooltip = tooltip;
}

/**
 * TFTD mode: much like click inversion, but does a colour swap rather than a palette shift.
 * @param mode set TFTD mode to this.
 */
void InteractiveSurface::setTFTDMode(bool mode)
{
	_tftdMode = mode;
}

/**
 * checks TFTD mode.
 * @return TFTD mode.
 */
bool InteractiveSurface::isTFTDMode() const
{
	return _tftdMode;
}

}
