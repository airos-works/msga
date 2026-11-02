// MSGA's tray icon on air/OS: a Deskbar add-on (loaded into Deskbar's own
// team, so it is small and links libbe alone, never msga itself).
//
// MSGA asks Deskbar to load it (BDeskbar::AddItem with this file) and
// removes it when its tray goes. The view tells MSGA it is there (kHello,
// with its messenger); MSGA sends the icon in a few sizes, the tooltip and
// the menu, and gets the clicks back: a primary click (kActivated), a menu
// choice (kMenuItem). Deskbar remembers add-ons across restarts, so a view
// that finds no MSGA running takes itself out again.
#include "haiku/deskbar_protocol.h"

#include <Bitmap.h>
#include <Deskbar.h>
#include <MenuItem.h>
#include <SeparatorItem.h>
#include <Message.h>
#include <Messenger.h>
#include <PopUpMenu.h>
#include <Roster.h>
#include <View.h>
#include <Window.h>

#include <cstring>
#include <dlfcn.h>
#include <vector>

using namespace plat::haiku::deskbar;

namespace {

// Device pixels per logical one (air/OS's display layout render scale).
float deviceScale() {
    using GetLayout             = status_t (*)(BMessage &);
    static GetLayout getLayout = reinterpret_cast<GetLayout>(
        dlsym(RTLD_DEFAULT, "_ZN8BPrivate18get_display_layoutER8BMessage")
    );
    BMessage layout;
    if (!getLayout || getLayout(layout) != B_OK)
        return 1;
    int32    best = 0;
    BMessage display;
    for (int32 i = 0; layout.FindMessage("display", i, &display) == B_OK; ++i) {
        int32 scale = 0;
        if (display.GetBool("enabled", true) && display.FindInt32("render scale", &scale) == B_OK &&
            scale > best)
            best = scale;
    }
    return best >= 100 && best <= 400 ? best / 100.0f : 1;
}

// Removes the item from Deskbar, which must not be asked from its own
// window thread.
int32 removeLater(void *) {
    snooze(200000);
    BDeskbar().RemoveItem(kItemName);
    return 0;
}

} // namespace

// Global, under its own name: Deskbar's shelf finds Instantiate by the
// archived class name.
class _EXPORT MsgaTrayView : public BView {
public:
    explicit MsgaTrayView(BRect frame)
        : BView(frame, kItemName, B_FOLLOW_LEFT | B_FOLLOW_TOP, B_WILL_DRAW) {}

    explicit MsgaTrayView(BMessage *archive) : BView(archive) {}

    ~MsgaTrayView() override { delete _bitmap; }

    // Out of line: Deskbar's shelf looks it up by name in this image. The
    // archive names no "add_on" signature: that would make the shelf load
    // MSGA itself into Deskbar should the lookup ever fail.
    static BArchivable *Instantiate(BMessage *archive);

    void AttachedToWindow() override {
        BView::AttachedToWindow();
        AdoptParentColors();
        SetDrawingMode(B_OP_ALPHA);
        SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
        hello();
    }

    void Draw(BRect) override {
        if (!_bitmap)
            return;
        const BRect b = Bounds();
        DrawBitmap(_bitmap, _bitmap->Bounds(), b, B_FILTER_BITMAP_BILINEAR);
    }

    void MouseDown(BPoint where) override {
        BMessage *current = Window()->CurrentMessage();
        int32     buttons = B_PRIMARY_MOUSE_BUTTON;
        if (current)
            current->FindInt32("buttons", &buttons);
        if (!_msga.IsValid()) {
            hello(); // MSGA gone (or never told us): ask again, or leave
            return;
        }
        if (buttons & B_SECONDARY_MOUSE_BUTTON) {
            showMenu(ConvertToScreen(where));
            return;
        }
        BMessage clicked(kActivated);
        _msga.SendMessage(&clicked);
    }

    void MessageReceived(BMessage *msg) override {
        switch (msg->what) {
        case kHello: { // MSGA answers with where it is
            BMessenger to;
            if (msg->FindMessenger("msga", &to) == B_OK)
                _msga = to;
            break;
        }
        case kIcon:
            setIcon(msg);
            break;
        case kTooltip: {
            const char *text = nullptr;
            SetToolTip(msg->FindString("text", &text) == B_OK ? text : nullptr);
            break;
        }
        case kMenu:
            _menu = *msg;
            break;
        case kGoodbye:
            _msga = BMessenger();
            break;
        default:
            BView::MessageReceived(msg);
        }
    }

private:
    void hello() {
        BMessenger msga(kSignature);
        if (!msga.IsValid()) {
            // Deskbar brought the item back after a restart, MSGA isn't
            // running: out again.
            resume_thread(spawn_thread(removeLater, "msga tray removal", B_LOW_PRIORITY, nullptr));
            return;
        }
        BMessage hi(kHello);
        hi.AddMessenger("item", BMessenger(this));
        msga.SendMessage(&hi);
    }

    void setIcon(BMessage *msg) {
        // The smallest size at least as big as the slot in device pixels.
        const float want  = (Bounds().Height() + 1) * deviceScale();
        int32       best  = -1, bestW = 0;
        int32       w = 0, h = 0; // w: each size's, h: the chosen one's
        for (int32 i = 0; msg->FindInt32("width", i, &w) == B_OK; ++i) {
            if (best < 0 || (bestW < want && w > bestW) || (w >= want && w < bestW)) {
                best  = i;
                bestW = w;
            }
        }
        if (best < 0 || msg->FindInt32("height", best, &h) != B_OK)
            return;
        const void *data = nullptr;
        ssize_t     size = 0;
        if (msg->FindData("pixels", B_RAW_TYPE, best, &data, &size) != B_OK ||
            size != ssize_t(bestW) * h * 4)
            return;
        auto *bitmap = new BBitmap(BRect(0, 0, bestW - 1, h - 1), B_RGBA32);
        if (bitmap->InitCheck() != B_OK) {
            delete bitmap;
            return;
        }
        for (int32 y = 0; y < h; ++y)
            std::memcpy(
                static_cast<uint8 *>(bitmap->Bits()) + size_t(y) * bitmap->BytesPerRow(),
                static_cast<const uint8 *>(data) + size_t(y) * bestW * 4, size_t(bestW) * 4
            );
        delete _bitmap;
        _bitmap = bitmap;
        Invalidate();
    }

    void addItems(BMenu *menu, const BMessage &items) {
        BMessage item;
        for (int32 i = 0; items.FindMessage("item", i, &item) == B_OK; ++i) {
            const int32 kind = item.GetInt32("kind", kKindAction);
            if (kind == kKindSeparator) {
                // None first, none doubled (a menu without workspaces
                // starts with the one meant to follow them); see the end.
                const int32 n = menu->CountItems();
                if (n > 0 && menu->ItemAt(n - 1)->Label() != nullptr &&
                    *menu->ItemAt(n - 1)->Label() != '\0')
                    menu->AddSeparatorItem();
                continue;
            }
            const char *label = item.GetString("label", "");
            if (kind == kKindSubmenu) {
                auto *sub = new BMenu(label);
                addItems(sub, item);
                menu->AddItem(sub);
                continue;
            }
            auto *chosen = new BMessage(kMenuItem);
            chosen->AddUInt32("id", item.GetUInt32("id", 0));
            auto *mi = new BMenuItem(label, chosen);
            mi->SetEnabled(item.GetBool("enabled", true));
            mi->SetMarked(item.GetBool("checked", false));
            mi->SetTarget(_msga);
            menu->AddItem(mi);
        }
    }

    void showMenu(BPoint where) {
        auto *menu = new BPopUpMenu("msga", false, false);
        menu->SetAsyncAutoDestruct(true);
        addItems(menu, _menu);
        while (menu->CountItems() > 0 &&
               dynamic_cast<BSeparatorItem *>(menu->ItemAt(menu->CountItems() - 1)) != nullptr)
            delete menu->RemoveItem(menu->CountItems() - 1);
        if (menu->CountItems() == 0) {
            delete menu;
            return;
        }
        menu->Go(where, true, true, BRect(where - BPoint(4, 4), where + BPoint(4, 4)), true);
    }

    BBitmap   *_bitmap = nullptr;
    BMessenger _msga;
    BMessage   _menu;
};


BArchivable *MsgaTrayView::Instantiate(BMessage *archive) {
    return validate_instantiation(archive, "MsgaTrayView") ? new MsgaTrayView(archive) : nullptr;
}

extern "C" _EXPORT BView *instantiate_deskbar_item(float maxWidth, float maxHeight) {
    (void)maxWidth;
    return new MsgaTrayView(BRect(0, 0, maxHeight - 1, maxHeight - 1));
}
