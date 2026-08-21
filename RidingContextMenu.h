#pragma once

// Minimal definition of Kenshi's ContextMenuGUI class, extracted from
// kenshi/gui/ContextMenu.h to avoid conflicting with the ContextMenu class
// already fully defined in PlayerInterface.h.

#include <kenshi/util/lektor.h>
#include <kenshi/util/hand.h>

#include <mygui/common/baselayout/BaseLayout.h>
#include <string>

class RootObject;

class ContextMenuGUI : public wraps::BaseLayout, public Ogre::GeneralAllocatedObject
{
public:
    // wraps::BaseLayout offset = 0x0, length = 0xA0
    // Ogre::AllocatedObject<Ogre::CategorisedAllocPolicy<0> > offset = 0xA1, length = 0x1
    ContextMenuGUI();// public RVA = 0x796540
    ContextMenuGUI* _CONSTRUCTOR();// public RVA = 0x796540
    virtual ~ContextMenuGUI();// public RVA = 0x793D70 vtable offset = 0x0
    void _DESTRUCTOR();// public RVA = 0x793D70 vtable offset = 0x0
    MyGUI::Widget* getMainWidget() const;// public RVA = 0x7BF260
    bool getVisible() const;// public RVA = 0x7BF270
    void setVisible(bool visible);// public RVA = 0x7938B0
    void show(const lektor<int>& ordersList, const std::string& _name, bool offset);// public RVA = 0x7A6D80
    void optionSelected(MyGUI::Widget* _sender, int _left, int _top, MyGUI::MouseButton _id);// public RVA = 0x7A6CB0
    hand contextMenuTarget; // 0xA8 Member
    std::string name; // 0xC8 Member
    MyGUI::TextBox* nameText; // 0xF0 Member
    MyGUI::Widget* optionsList; // 0xF8 Member
    MyGUI::types::TCoord<int> optionCoords; // 0x100 Member
    MyGUI::types::TCoord<int> buttonCoords; // 0x110 Member
    MyGUI::types::TCoord<int> valueCoords; // 0x120 Member
};
