// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_BROWSER_ACCESSIBILITY_BROWSER_ACCESSIBILITY_H_
#define CONTENT_BROWSER_ACCESSIBILITY_BROWSER_ACCESSIBILITY_H_

#include <map>
#include <utility>
#include <vector>

#include "base/basictypes.h"
#include "build/build_config.h"
#include "content/common/accessibility_node_data.h"
#include "content/common/content_export.h"

#if defined(OS_MACOSX) && __OBJC__
@class BrowserAccessibilityCocoa;
#endif

namespace content {
class BrowserAccessibilityManager;
#if defined(OS_WIN)
class BrowserAccessibilityWin;
#elif defined(TOOLKIT_GTK)
class BrowserAccessibilityGtk;
#endif

typedef std::map<AccessibilityNodeData::BoolAttribute, bool> BoolAttrMap;
typedef std::map<AccessibilityNodeData::FloatAttribute, float> FloatAttrMap;
typedef std::map<AccessibilityNodeData::IntAttribute, int> IntAttrMap;
typedef std::map<AccessibilityNodeData::StringAttribute, string16>
    StringAttrMap;

////////////////////////////////////////////////////////////////////////////////
//
// BrowserAccessibility
//
// Class implementing the cross platform interface for the Browser-Renderer
// communication of accessibility information, providing accessibility
// to be used by screen readers and other assistive technology (AT).
//
// An implementation for each platform handles platform specific accessibility
// APIs.
//
////////////////////////////////////////////////////////////////////////////////
class CONTENT_EXPORT BrowserAccessibility {
 public:
  // Creates a platform specific BrowserAccessibility. Ownership passes to the
  // caller.
  static BrowserAccessibility* Create();

  virtual ~BrowserAccessibility();

  // Detach all descendants of this subtree and push all of the node pointers,
  // including this node, onto the end of |nodes|.
  virtual void DetachTree(std::vector<BrowserAccessibility*>* nodes);

  // Perform platform-specific initialization. This can be called multiple times
  // during the lifetime of this instance after the members of this base object
  // have been reset with new values from the renderer process.
  // Child dependent initialization can be done here.
  virtual void PostInitialize() {}

  // Returns true if this is a native platform-specific object, vs a
  // cross-platform generic object.
  virtual bool IsNative() const;

  // Initialize the tree structure of this object.
  void InitializeTreeStructure(
      BrowserAccessibilityManager* manager,
      BrowserAccessibility* parent,
      int32 child_id,
      int32 renderer_id,
      int32 index_in_parent);

  // Initialize this object's data.
  void InitializeData(const AccessibilityNodeData& src);

  // Add a child of this object.
  void AddChild(BrowserAccessibility* child);

  void SwapChildren(std::vector<BrowserAccessibility*>& children);

  // Update the parent and index in parent if this node has been moved.
  void UpdateParent(BrowserAccessibility* parent, int index_in_parent);

  // Return true if this object is equal to or a descendant of |ancestor|.
  bool IsDescendantOf(BrowserAccessibility* ancestor);

  // Returns the parent of this object, or NULL if it's the root.
  BrowserAccessibility* parent() { return parent_; }

  // Returns the number of children of this object.
  uint32 child_count() const { return children_.size(); }

  // Return a pointer to the child with the given index.
  BrowserAccessibility* GetChild(uint32 child_index);

  // Return the previous sibling of this object, or NULL if it's the first
  // child of its parent.
  BrowserAccessibility* GetPreviousSibling();

  // Return the next sibling of this object, or NULL if it's the last child
  // of its parent.
  BrowserAccessibility* GetNextSibling();

  // Returns the bounds of this object in coordinates relative to the
  // top-left corner of the overall web area.
  gfx::Rect GetLocalBoundsRect();

  // Returns the bounds of this object in screen coordinates.
  gfx::Rect GetGlobalBoundsRect();

  // Returns the deepest descendant that contains the specified point
  // (in global screen coordinates).
  BrowserAccessibility* BrowserAccessibilityForPoint(const gfx::Point& point);

  // Marks this object for deletion, releases our reference to it, and
  // recursively calls Destroy() on its children.  May not delete
  // immediately due to reference counting.
  //
  // Reference counting is used on some platforms because the
  // operating system may hold onto a reference to a BrowserAccessibility
  // object even after we're through with it. When a BrowserAccessibility
  // has had Destroy() called but its reference count is not yet zero,
  // queries on this object return failure
  virtual void Destroy();

  // Subclasses should override this to support platform reference counting.
  virtual void NativeAddReference() { }

  // Subclasses should override this to support platform reference counting.
  virtual void NativeReleaseReference();

  //
  // Accessors
  //

  const BoolAttrMap& bool_attributes() const {
    return bool_attributes_;
  }

  const FloatAttrMap& float_attributes() const {
    return float_attributes_;
  }

  const IntAttrMap& int_attributes() const {
    return int_attributes_;
  }

  const StringAttrMap& string_attributes() const {
    return string_attributes_;
  }

  int32 child_id() const { return child_id_; }
  const std::vector<BrowserAccessibility*>& children() const {
    return children_;
  }
  const std::vector<std::pair<string16, string16> >& html_attributes() const {
    return html_attributes_;
  }
  int32 index_in_parent() const { return index_in_parent_; }
  const std::vector<int32>& indirect_child_ids() const {
    return indirect_child_ids_;
  }
  const std::vector<int32>& line_breaks() const {
    return line_breaks_;
  }
  const std::vector<int32>& cell_ids() const {
    return cell_ids_;
  }
  const std::vector<int32>& unique_cell_ids() const {
    return unique_cell_ids_;
  }
  gfx::Rect location() const { return location_; }
  BrowserAccessibilityManager* manager() const { return manager_; }
  const string16& name() const { return name_; }
  int32 renderer_id() const { return renderer_id_; }
  int32 role() const { return role_; }
  const string16& role_name() const { return role_name_; }
  int32 state() const { return state_; }
  const string16& value() const { return value_; }
  bool instance_active() const { return instance_active_; }

#if defined(OS_MACOSX) && __OBJC__
  BrowserAccessibilityCocoa* ToBrowserAccessibilityCocoa();
#elif defined(OS_WIN)
  BrowserAccessibilityWin* ToBrowserAccessibilityWin();
#elif defined(TOOLKIT_GTK)
  BrowserAccessibilityGtk* ToBrowserAccessibilityGtk();
#endif

  // Retrieve the value of a bool attribute from the bool attribute
  // map and returns true if found.
  bool GetBoolAttribute(
      AccessibilityNodeData::BoolAttribute attr, bool* value) const;

  // Retrieve the value of a float attribute from the float attribute
  // map and returns true if found.
  bool GetFloatAttribute(AccessibilityNodeData::FloatAttribute attr,
                         float* value) const;

  // Retrieve the value of an integer attribute from the integer attribute
  // map and returns true if found.
  bool GetIntAttribute(AccessibilityNodeData::IntAttribute attribute,
                       int* value) const;

  // Retrieve the value of a string attribute from the attribute map and
  // returns true if found.
  bool GetStringAttribute(
      AccessibilityNodeData::StringAttribute attribute, string16* value) const;

  // Retrieve the value of a html attribute from the attribute map and
  // returns true if found.
  bool GetHtmlAttribute(const char* attr, string16* value) const;

  // Utility method to handle special cases for ARIA booleans, tristates and
  // booleans which have a "mixed" state.
  //
  // Warning: the term "Tristate" is used loosely by the spec and here,
  // as some attributes support a 4th state.
  //
  // The following attributes are appropriate to use with this method:
  // aria-selected  (selectable)
  // aria-grabbed   (grabbable)
  // aria-expanded  (expandable)
  // aria-pressed   (toggleable/pressable) -- supports 4th "mixed" state
  // aria-checked   (checkable) -- supports 4th "mixed state"
  bool GetAriaTristate(const char* attr_name,
                       bool* is_defined,
                       bool* is_mixed) const;

  // Returns true if the bit corresponding to the given state enum is 1.
  bool HasState(AccessibilityNodeData::State state_enum) const;

  // Returns true if this node is an editable text field of any kind.
  bool IsEditableText() const;

  // Append the text from this node and its children.
  string16 GetTextRecursive() const;

 protected:
  // Perform platform specific initialization. This can be called multiple times
  // during the lifetime of this instance after the members of this base object
  // have been reset with new values from the renderer process.
  // Perform child independent initialization in this method.
  virtual void PreInitialize() {}

  BrowserAccessibility();

  // The manager of this tree of accessibility objects; needed for
  // global operations like focus tracking.
  BrowserAccessibilityManager* manager_;

  // The parent of this object, may be NULL if we're the root object.
  BrowserAccessibility* parent_;

  // The ID of this object; globally unique within the browser process.
  int32 child_id_;

  // The index of this within its parent object.
  int32 index_in_parent_;

  // The ID of this object in the renderer process.
  int32 renderer_id_;

  // The children of this object.
  std::vector<BrowserAccessibility*> children_;

  // Accessibility metadata from the renderer
  string16 name_;
  string16 value_;
  BoolAttrMap bool_attributes_;
  IntAttrMap int_attributes_;
  FloatAttrMap float_attributes_;
  StringAttrMap string_attributes_;
  std::vector<std::pair<string16, string16> > html_attributes_;
  int32 role_;
  int32 state_;
  string16 role_name_;
  gfx::Rect location_;
  std::vector<int32> indirect_child_ids_;
  std::vector<int32> line_breaks_;
  std::vector<int32> cell_ids_;
  std::vector<int32> unique_cell_ids_;

  // BrowserAccessibility objects are reference-counted on some platforms.
  // When we're done with this object and it's removed from our accessibility
  // tree, a client may still be holding onto a pointer to this object, so
  // we mark it as inactive so that calls to any of this object's methods
  // immediately return failure.
  bool instance_active_;

 private:
  DISALLOW_COPY_AND_ASSIGN(BrowserAccessibility);
};

}  // namespace content

#endif  // CONTENT_BROWSER_ACCESSIBILITY_BROWSER_ACCESSIBILITY_H_
