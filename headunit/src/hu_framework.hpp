// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
//
// Minimal declarations of the HU Framework: the head unit's RHMI app framework, libAppFramework.A.so and
// libCoreFramework.A.so. It ships no headers: only names and parameter types matter here, because they produce the
// mangled symbols the two libraries export. That is also why the namespace has to stay `bmw`. Nothing in this file
// is defined by us except the small inline pieces (Map's constructor, Variant::ObjectContainer).
//
// Layouts were read from the head unit's own PluginApps (libSecurityMenu.so, libActiveSeating.so). Sizes that are only known
// as "at least" are padded.
#ifndef NBTAPP_HU_FRAMEWORK_HPP
#define NBTAPP_HU_FRAMEWORK_HPP

#include <stddef.h>

namespace bmw {

class IDispatcher;
class Bundle;

// vtable { ~D1, ~D0, run() }. The framework exports the vtable, the type info and the destructors D1 and D0, and
// not the base-object destructor (D2) that a derived class's destructor calls: nothing here derives from it. It is
// declared in full only so that its type info can be named; a runnable of ours is built by hand (nbt_app.cpp).
class Runnable {
public:
    virtual ~Runnable();                     // _ZN3bmw8RunnableD1Ev, _ZN3bmw8RunnableD0Ev
    virtual void run() = 0;
};

class Parcel {
public:
    explicit Parcel(bool value);             // _ZN3bmw6ParcelC1Eb, 0x14 bytes, polymorphic
    ~Parcel();                               // _ZN3bmw6ParcelD1Ev
    void readBool(bool& value) const;        // _ZNK3bmw6Parcel8readBoolERb
private:
    unsigned char m_storage[32];             // >= sizeof(bmw::Parcel) (0x14)
};

enum RHMIInterfaceVersion { RHMI_INTERFACE_VERSION_1 = 1 };

template <typename T> class BasicString {
public:
    BasicString(const T* s);                 // _ZN3bmw11BasicStringIcEC1EPKc
    BasicString(const BasicString& other);   // _ZN3bmw11BasicStringIcEC1ERKS1_
    ~BasicString();                          // _ZN3bmw11BasicStringIcED1Ev
    const T* raw() const { return m_data; }  // the data pointer is the only member
private:
    T* m_data;
};

// Text-DB reference: { u32 id, u32 type 0x28 }, trivially destructible.
class TextID {
public:
    explicit TextID(unsigned id);            // _ZN3bmw6TextIDC1Ej
private:
    unsigned m_id;
    unsigned m_type;
};

// Image-DB reference: { u32 id, u32 type 0x2b }.
class ImageID {
public:
    explicit ImageID(unsigned id);           // _ZN3bmw7ImageIDC1Ej
private:
    unsigned m_id;
    unsigned m_type;
};

// ---- list-model table (raListModel): DataTableBase over a Map<unsigned, Variant> ----------------------------------
//   Variant          = one pointer to an IObjectContainer (deleted through vtable slot 1 by ~Variant)
//   IObjectContainer = vtable { ~D1, ~D0, type() const, clone() const }; ObjectContainer<TextID>: type() = 0x28
//   Map<unsigned,Variant> = { assoc** table, u32 hashSize 17, u32 count, assoc* free, MemoryBlock* blocks, u32 blockSize 10 }
//   DataTableBase(Map*, columns) = 8 bytes; cell (row, col) is map key row * columns + col
class Variant {
public:
    class IObjectContainer {
    public:
        virtual ~IObjectContainer() {}
        virtual int type() const = 0;
        virtual IObjectContainer* clone() const = 0;
    };
    template <typename T> class ObjectContainer : public IObjectContainer {
    public:
        explicit ObjectContainer(const T& v) : m_value(v) {}
        virtual int type() const;
        virtual IObjectContainer* clone() const { return new ObjectContainer<T>(m_value); }
    private:
        T m_value;
    };
    ~Variant();                              // _ZN3bmw7VariantD1Ev (deletes the container)
    IObjectContainer* m_obj;
};
template <> inline int Variant::ObjectContainer<TextID>::type() const { return 0x28; }

template <typename K, typename V> class Map {
public:
    Map() : m_table(0), m_hashSize(17), m_count(0), m_free(0), m_blocks(0), m_blockSize(10) {}
    ~Map();                                  // _ZN3bmw3MapIjNS_7VariantEED1Ev
    V& operator[](const K& key);             // _ZN3bmw3MapIjNS_7VariantEEixERKj
private:
    void* m_table;
    unsigned m_hashSize, m_count;
    void* m_free;
    void* m_blocks;
    unsigned m_blockSize;
};

class DataTableBase {
public:
    DataTableBase(Map<unsigned, Variant>* cells, unsigned columns);   // _ZN3bmw13DataTableBaseC1EPNS_3MapIjNS_7VariantEEEj
private:
    unsigned char m_storage[16];             // 8 used
};

// Two words, which addHandler copies into the app's own table. The framework calls stub(object, argument), and
// only when both words are set: PluginApp::handleEvent, and what it does for an action, look the delegate up and
// return before the call when the object is null, as they do for an id that has no handler. A plain function
// therefore needs an object all the same; the head unit's own apps and ours hand it the app.
template <typename R, typename A1, typename A2, typename A3, typename A4> struct Delegate {
    void* object;
    R (*stub)(void* object, A1 a1);
};
typedef Delegate<bool, const Parcel&, void, void, void> ActionDelegate;
typedef Delegate<bool, const BasicString<unsigned short>&, void, void, void> CdsDelegate;

const size_t kPluginAppStorage = 0x200;      // SecurityMenu's own members start at 0x160

// PluginApp vtable: 34 slots from the address point, no pure virtuals.
enum { kSlotOnError = 21, kSlotOnInit = 22, kSlotOnFinalize = 23, kPluginAppVtblSlots = 34 };

class PluginApp {
public:
    PluginApp(IDispatcher& dispatcher, BasicString<char> name, unsigned major, unsigned minor,
              RHMIInterfaceVersion rhmi, const Bundle& params, const Bundle& config);
    // <raAction id="..."> handler.
    void addHandler(unsigned actionId, const ActionDelegate& handler);
    // Notification handler for one of our hmiStates. type 1 = focus, 11 = visible; the Parcel carries one bool.
    void addHandler(unsigned id, unsigned type, const ActionDelegate& handler);
    // Fires one of our XML <events>. A focusEvent whose targetModel is a NativeStateID raIntModel makes HmiMain3D
    // request that native state; one with target="<hmiState>" opens that state of ours.
    void triggerEvent(unsigned eventId);
    void post(Runnable* runnable, unsigned delayMs);   // runs runnable->run() on the app thread after delayMs
    void updateModel(unsigned modelId, const TextID& text);          // textIdModel: re-point at another text
    void updateModel(unsigned modelId, const ImageID& image);        // imageIdModel
    void updateModel(unsigned modelId, bool value);                  // raBoolModel (checkbox)
    void updateModel(unsigned modelId, const DataTableBase& table);  // raListModel
    // Component property: 1 enabled, 2 selectable, 3 visible, 8 waiting animation (iDrive's busy ring).
    void setProperty(unsigned componentId, unsigned propertyId, const Parcel& value);
    // Car data (CDS) subscription; the value arrives as UTF-16 JSON text.
    void cdsAddPropertyChangedEventHandler(BasicString<char>& property, unsigned id, const CdsDelegate& handler);
    bool onInit();                           // base implementations, called from our overrides
    void onFinalize();
private:
    unsigned char m_storage[kPluginAppStorage];
};

} // namespace bmw

#endif
