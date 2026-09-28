#pragma once
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
#include <SDL.h>

namespace OpenXcom
{

class InteractiveSurface;

/**
 * Container for all the information associated with a
 * given user action, like mouse clicks, key presses, etc.
 * @note Called action because event is reserved.
 */
class Action
{
private:
	SDL_Event *_ev;
	double _scaleX, _scaleY;
	int _topBlackBand, _leftBlackBand, _mouseX, _mouseY, _surfaceX, _surfaceY;
	double _surfaceScaleX, _surfaceScaleY;
	int _presentationAnchorX, _presentationAnchorY;
	double _presentationScaleX, _presentationScaleY;
	bool _logicalMouseOverrideEnabled;
	double _logicalMouseOverrideX, _logicalMouseOverrideY;
	InteractiveSurface *_sender;
public:
	/// Creates an action with given event data.
	Action(SDL_Event *ev, double scaleX, double scaleY, int topBlackBand, int leftBlackBand);
	/// Cleans up the action.
	~Action();
	/// Gets the screen's X scale.
	double getXScale() const;
	/// Gets the screen's Y scale.
	double getYScale() const;
	/// Sets the action as a mouse action. surfaceX/Y are the surface's ORIGINAL logical coordinates.
	void setMouseAction(int mouseX, int mouseY, int surfaceX, int surfaceY, double surfaceScaleX = 1.0, double surfaceScaleY = 1.0);
	/// Sets the presentation transform used to map displayed UI coordinates back to original logical coordinates.
	void setPresentationTransform(int anchorX, int anchorY, double scaleX = 1.0, double scaleY = 1.0);
	/// Overrides absolute mouse coordinates with an explicit presentation-space logical position.
	/// Used by fixed UI spaces whose physical transform is intentionally independent from Screen/World scale.
	void setLogicalMouseOverride(double logicalX, double logicalY);
	/// Clears an explicit presentation-space logical mouse override.
	void clearLogicalMouseOverride();
	/// Gets if the action is a mouse action.
	bool isMouseAction() const;
	/// Gets the top black band height.
	int getTopBlackBand() const;
	/// Gets the left black band width.
	int getLeftBlackBand() const;
	/// Gets the mouse's X position.
	int getXMouse() const;
	/// Gets the mouse's Y position.
	int getYMouse() const;
	/// Gets the mouse position in the displayed logical screen (before per-surface UI transform).
	double getDisplayXMouse() const;
	double getDisplayYMouse() const;
	/// Gets the mouse's absolute X position in the current surface's original logical coordinate system.
	double getAbsoluteXMouse() const;
	/// Gets the mouse's absolute Y position in the current surface's original logical coordinate system.
	double getAbsoluteYMouse() const;
	/// Gets the mouse's relative X position.
	double getRelativeXMouse() const;
	/// Gets the mouse's relative Y position.
	double getRelativeYMouse() const;
	/// Gets the sender of the action.
	InteractiveSurface *getSender() const;
	/// Sets the sender of the action.
	void setSender(InteractiveSurface *sender);
	/// Gets the details of the action.
	SDL_Event *getDetails() const;
};

}
