#pragma once

// RaceMenu's SKEE interface, in header form so its sources are not included.

namespace SKEE
{
	class IPluginInterface
	{
	public:
		IPluginInterface() = default;
		virtual ~IPluginInterface() = default;

		virtual std::uint32_t GetVersion() = 0;
		virtual void Revert() = 0;
	};

	class IInterfaceMap
	{
	public:
		virtual IPluginInterface* QueryInterface(const char* a_name) = 0;
		virtual bool AddInterface(const char* a_name, IPluginInterface* a_interface) = 0;
		virtual IPluginInterface* RemoveInterface(const char* a_name) = 0;
	};

	struct InterfaceExchangeMessage
	{
		enum : std::uint32_t
		{
			kExchangeInterface = 0x9E3779B9
		};

		IInterfaceMap* interfaceMap{ nullptr };
	};

	class IBodyMorphInterface : public IPluginInterface
	{
	public:
		class MorphKeyVisitor
		{
		public:
			virtual void Visit(const char*, float) = 0;
		};

		class StringVisitor
		{
		public:
			virtual void Visit(const char*) = 0;
		};

		class ActorVisitor
		{
		public:
			virtual void Visit(RE::TESObjectREFR*) = 0;
		};

		class MorphValueVisitor
		{
		public:
			virtual void Visit(RE::TESObjectREFR*, const char*, const char*, float) = 0;
		};

		class MorphVisitor
		{
		public:
			virtual void Visit(RE::TESObjectREFR*, const char*) = 0;
		};

		virtual void SetMorph(RE::TESObjectREFR*, const char*, const char*, float) = 0;
		virtual float GetMorph(RE::TESObjectREFR*, const char*, const char*) = 0;
		virtual void ClearMorph(RE::TESObjectREFR*, const char*, const char*) = 0;
		virtual float GetBodyMorphs(RE::TESObjectREFR*, const char*) = 0;
		virtual void ClearBodyMorphNames(RE::TESObjectREFR*, const char*) = 0;
		virtual void VisitMorphs(RE::TESObjectREFR*, MorphVisitor&) = 0;
		virtual void VisitKeys(RE::TESObjectREFR*, const char*, MorphKeyVisitor&) = 0;
		virtual void VisitMorphValues(RE::TESObjectREFR*, MorphValueVisitor&) = 0;
		virtual void ClearMorphs(RE::TESObjectREFR*) = 0;
		virtual void ApplyVertexDiff(RE::TESObjectREFR*, RE::NiAVObject*, bool = false) = 0;
		virtual void ApplyBodyMorphs(RE::TESObjectREFR*, bool = true) = 0;
		virtual void UpdateModelWeight(RE::TESObjectREFR*, bool = false) = 0;
		virtual void SetCacheLimit(std::size_t) = 0;
		virtual bool HasMorphs(RE::TESObjectREFR*) = 0;
		virtual std::uint32_t EvaluateBodyMorphs(RE::TESObjectREFR*) = 0;
		virtual bool HasBodyMorph(RE::TESObjectREFR*, const char*, const char*) = 0;
		virtual bool HasBodyMorphName(RE::TESObjectREFR*, const char*) = 0;
		virtual bool HasBodyMorphKey(RE::TESObjectREFR*, const char*) = 0;
		virtual void ClearBodyMorphKeys(RE::TESObjectREFR*, const char*) = 0;
		virtual void VisitStrings(StringVisitor&) = 0;
		virtual void VisitActors(ActorVisitor&) = 0;
		virtual std::size_t ClearMorphCache() = 0;
	};
}
