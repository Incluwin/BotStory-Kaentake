#pragma once

class CUIToolTip;

// The module set_items (setitems.cpp): what tooltip.cpp's equip tooltip hooks call. Each does nothing while the
// module is not attached or inert, so tool_tip keeps its own behaviour.
namespace setitems {

// From CUIToolTip::SetToolTip_Equip_Basic_hook, after its own rows: adds SET EFFECT / PIECES EQUIPPED for a set item.
void OnEquipBasic(CUIToolTip* pTip, int nItemID);

// From CUIToolTip__DrawToolTip_Equip_hook, after the game drew it: shows the set's own tooltip beside it.
void OnDrawEquip(CUIToolTip* pTip, int nItemID);

}  // namespace setitems
