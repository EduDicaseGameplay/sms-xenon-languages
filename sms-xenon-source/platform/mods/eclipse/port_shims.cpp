// Functions BetterSunshineEngine and Eclipse call out of line that the decomp
// only has inline in its headers, exported under the names those mods'
// objects reference. Each one runs the decomp's own inline code (placement new
// for constructors), so the behaviour is the port's. Compiled with the game's
// flags and headers; 32-bit only (on i386 a member function is an ordinary
// function taking `this` first, which is what these are).
#include <new>

#include <Enemy/Graph.hpp>
#include <JSystem/JDrama/JDRActor.hpp>
#include <JSystem/JDrama/JDRNameRef.hpp>
#include <JSystem/JDrama/JDRNameRefGen.hpp>
#include <JSystem/JDrama/JDRViewObj.hpp>
#include <JSystem/JGeometry/JGQuat4.hpp>
#include <JSystem/JGeometry/JGVec3.hpp>
#include <JSystem/JSupport/JSUInputStream.hpp>
#include <JSystem/JSupport/JSUIosBase.hpp>
#include <JSystem/JUtility/JUTColor.hpp>
#include <JSystem/JUtility/JUTPoint.hpp>
#include <JSystem/JUtility/JUTRect.hpp>
#include <Map/MapData.hpp>
#include <MarioUtil/MathUtil.hpp>
#include <Strategic/Nerve.hpp>
#include <Strategic/LiveActor.hpp>
#include <Strategic/spcinterp.hpp>
#include <System/FlagManager.hpp>
#include <System/GameSequence.hpp>
#include <System/ScenarioArchiveName.hpp>
#include <Enemy/Emario.hpp>

// sym is the mods' C++ name; asm labels are literal, so add the platform's
// prefix (Mach-O and 32-bit COFF put an underscore before every C name).
#define SHIM_STR2(x) #x
#define SHIM_STR(x) SHIM_STR2(x)
#define SHIM(ret, name, sym, ...) \
	extern "C" ret name(__VA_ARGS__) __asm__(SHIM_STR(__USER_LABEL_PREFIX__) sym); ret name(__VA_ARGS__)

typedef JGeometry::TVec3<f32> Vec3f;

// Abstract bases: their base-object constructor only runs inside a derived
// class's constructor, which sets its own vtable right after, so a concrete
// stand-in with no data of its own constructs them.
namespace {
struct ConcreteInputStream : public JSUInputStream {
	virtual int getAvailable() const { return 0; }
	virtual int readData(void*, s32) { return 0; }
};
struct ConcreteViewObj : public JDrama::TViewObj {
	ConcreteViewObj(const char* name) : JDrama::TViewObj(name) { }
	virtual void perform(u32, JDrama::TGraphics*) { }
};
} // namespace
typedef JDrama::TFlagT<u16> FlagU16;

// Constructors and destructors (complete and base-object forms alike: none
// of these classes has virtual bases). Each returns this: on arm64 Apple
// platforms a caller takes a constructor's or non-deleting destructor's
// result as the object (BSE's new SettingsDirector() used what TNameRef's
// returned), and elsewhere it is ignored.
SHIM(JSUIosBase*, shim_JSUIosBase_ctor, "_ZN10JSUIosBaseC2Ev", JSUIosBase* self) { new (self) JSUIosBase; return self; }
SHIM(JSUInputStream*, shim_JSUInputStream_ctor, "_ZN14JSUInputStreamC2Ev", JSUInputStream* self)
{
	new (self) ConcreteInputStream;
	return self;
}
SHIM(JDrama::TNameRef*, shim_TNameRef_ctor, "_ZN6JDrama8TNameRefC2EPKc", JDrama::TNameRef* self, const char* name)
{
	new (self) JDrama::TNameRef(name);
	return self;
}
SHIM(JDrama::TActor*, shim_TActor_ctor, "_ZN6JDrama6TActorC2EPKc", JDrama::TActor* self, const char* name)
{
	new (self) JDrama::TActor(name);
	return self;
}
SHIM(JDrama::TViewObj*, shim_TViewObj_ctor, "_ZN6JDrama8TViewObjC2EPKc", JDrama::TViewObj* self, const char* name)
{
	new (self) ConcreteViewObj(name);
	return self;
}
// A destructor here is the symbol it would call (its own name: the compiler
// took that for endless recursion and made the shim a trap, hit as Eclipse's
// nerves are destroyed when the game exits), so each does the work itself:
// TViewObj's implicit destructor runs TNameRef's, TNerveBase's is empty.
SHIM(JDrama::TViewObj*, shim_TViewObj_dtor, "_ZN6JDrama8TViewObjD2Ev", JDrama::TViewObj* self)
{
	self->JDrama::TNameRef::~TNameRef();
	return self;
}
SHIM(TNerveBase<TLiveActor>*, shim_TNerveBase_dtor, "_ZN10TNerveBaseI10TLiveActorED2Ev", TNerveBase<TLiveActor>* self)
{
	return self;
}
SHIM(TScenarioArchiveName*, shim_TScenarioArchiveName_copy, "_ZN20TScenarioArchiveNameC1ERKS_", TScenarioArchiveName* self,
     const TScenarioArchiveName* other)
{
	new (self) TScenarioArchiveName(*other);
	return self;
}
SHIM(FlagU16*, shim_TFlagT_copy, "_ZN6JDrama6TFlagTItEC1ERKS1_", FlagU16* self, const FlagU16* other)
{
	new (self) FlagU16(*other);
	return self;
}
SHIM(JUTRect*, shim_JUTRect_ctor4, "_ZN7JUTRectC1Eiiii", JUTRect* self, int x1, int y1, int x2, int y2)
{
	new (self) JUTRect(x1, y1, x2, y2);
	return self;
}
SHIM(JUTRect*, shim_JUTRect_ctor0, "_ZN7JUTRectC1Ev", JUTRect* self) { new (self) JUTRect; return self; }
SHIM(JUTRect*, shim_JUTRect_copy, "_ZN7JUTRectC1ERKS_", JUTRect* self, const JUTRect* other)
{
	new (self) JUTRect(*other);
	return self;
}
SHIM(JUTPoint*, shim_JUTPoint_ctor0, "_ZN8JUTPointC1Ev", JUTPoint* self) { new (self) JUTPoint; return self; }
SHIM(JUtility::TColor*, shim_TColor_ctor0, "_ZN8JUtility6TColorC1Ev", JUtility::TColor* self)
{
	new (self) JUtility::TColor;
	return self;
}
SHIM(TSpcSlice*, shim_TSpcSlice_ctor0, "_ZN9TSpcSliceC1Ev", TSpcSlice* self) { new (self) TSpcSlice; return self; }

// Member functions
SHIM(void, shim_TColor_set, "_ZN8JUtility6TColor3setEhhhh", JUtility::TColor* self, u8 r, u8 g, u8 b,
     u8 a)
{
	self->set(r, g, b, a);
}
SHIM(void, shim_TGameSequence_set, "_ZN13TGameSequence3setEhhN6JDrama6TFlagTItEE", TGameSequence* self,
     u8 area, u8 episode, const FlagU16* flag) // non-trivially copyable: passed by reference
{
	self->set(area, episode, *flag);
}
SHIM(JDrama::TNameRefGen*, shim_TNameRefGen_getInstance, "_ZN6JDrama11TNameRefGen11getInstanceEv", void)
{
	return JDrama::TNameRefGen::getInstance();
}
SHIM(JDrama::TNameRef*, shim_TNameRefGen_getRoot, "_ZN6JDrama11TNameRefGen14getRootNameRefEv",
     JDrama::TNameRefGen* self)
{
	return self->getRootNameRef();
}
SHIM(void, shim_TVec3_add, "_ZN9JGeometry5TVec3IfE3addERKS1_", Vec3f* self, const Vec3f* b) { self->add(*b); }
SHIM(void, shim_TVec3_sub1, "_ZN9JGeometry5TVec3IfE3subERKS1_", Vec3f* self, const Vec3f* b) { self->sub(*b); }
SHIM(void, shim_TVec3_sub2, "_ZN9JGeometry5TVec3IfE3subERKS1_S3_", Vec3f* self, const Vec3f* a,
     const Vec3f* b)
{
	self->sub(*a, *b);
}
SHIM(void, shim_TVec3_scale1, "_ZN9JGeometry5TVec3IfE5scaleEf", Vec3f* self, f32 s) { self->scale(s); }
SHIM(void, shim_TVec3_scale2, "_ZN9JGeometry5TVec3IfE5scaleEfRKS1_", Vec3f* self, f32 s, const Vec3f* b)
{
	self->scale(s, *b);
}
SHIM(void, shim_TQuat4_rotate, "_ZNK9JGeometry6TQuat4IfE6rotateERKNS_5TVec3IfEERS3_",
     const JGeometry::TQuat4<f32>* self, const Vec3f* v, Vec3f* out)
{
	self->rotate(*v, *out);
}
SHIM(bool, shim_TBGCheckData_isMarioThrough, "_ZNK12TBGCheckData14isMarioThroughEv", const TBGCheckData* self)
{
	return self->isMarioThrough();
}
SHIM(bool, shim_TBGCheckData_isWaterSurface, "_ZNK12TBGCheckData14isWaterSurfaceEv", const TBGCheckData* self)
{
	return self->isWaterSurface();
}
SHIM(const Vec3f*, shim_TBGCheckData_getNormal, "_ZNK12TBGCheckData9getNormalEv", const TBGCheckData* self)
{
	return &self->getNormal();
}
SHIM(int, shim_TGraphTracer_getCurGraphIndex, "_ZNK12TGraphTracer16getCurGraphIndexEv", const TGraphTracer* self)
{
	return self->getCurGraphIndex();
}
SHIM(int, shim_TSpcSlice_getDataInt, "_ZNK9TSpcSlice10getDataIntEv", const TSpcSlice* self)
{
	return self->getDataInt();
}
SHIM(f32, shim_TSpcSlice_getDataFloat, "_ZNK9TSpcSlice12getDataFloatEv", const TSpcSlice* self)
{
	return self->getDataFloat();
}

// Free functions
SHIM(f32, shim_MsWrap_f, "_Z6MsWrapIfET_S0_S0_S0_", f32 t, f32 l, f32 r) { return MsWrap<f32>(t, l, r); }
SHIM(void, shim_SpcTrace, "_Z8SpcTracePKcz", const char*, ...) { }

// Retail function names BetterSunshineEngine declares extern "C"
extern "C" void setShineFlag__12TFlagManagerFUc(TFlagManager* self, u16 flag)
{
	self->setShineFlag((u8)flag);
}

// BetterSunshineEngine's shadowMarioInitHandler, in place of TEMario::loadAfter's
// SMS_isMultiPlayerMap call, loads Shadow Mario from the TEMario in r31 at its
// retail offset (lwz r, 0x150(r31)): the port's hook passes the TEMario
// (zz-modhook-50) and the mod reads the member here (fixup_sources.py).
extern "C" void* sms_mod_emario_mario(void* emario) { return static_cast<TEMario*>(emario)->mEnemyMario; }
