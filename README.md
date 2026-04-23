# AnimBP2FP - Animation Blueprint to Functional Programming

**UE5.6 Plugin for converting Animation Blueprints to/from functional DSL**

---

## 🎯 What is this?

AnimBP2FP is a Unreal Engine 5.6 plugin that enables bidirectional conversion between Animation Blueprints and a functional programming DSL (AnimLang).

**Key Features**:
- ✅ Export Animation Blueprints to text-based DSL
- ✅ Import DSL back to Animation Blueprints
- ✅ Version control friendly (text vs binary)
- ✅ Type-safe with Haskell-inspired type system
- ✅ S-expression syntax (Lisp-like)

**Functional Purity**: ~70% (state machines have side effects)

---

## 📦 Installation

### Method 1: Copy to Project Plugins
```bash
# Copy this folder to your UE5.6 project
YourProject/
└── Plugins/
    └── AnimBP2FP/        # This plugin
```

### Method 2: Copy to Engine Plugins
```bash
# Copy to engine-wide plugins
UE_5.6/
└── Engine/
    └── Plugins/
        └── AnimBP2FP/    # This plugin
```

Then:
1. Regenerate project files (right-click .uproject → Generate Visual Studio project files)
2. Rebuild project
3. Enable plugin in UE Editor: Edit → Plugins → Search "AnimBP2FP"

---

## 🚀 Quick Start

### Export Animation Blueprint to DSL

```cpp
// C++ API
#include "AnimBPExporter.h"

UAnimBlueprint* AnimBP = LoadObject<UAnimBlueprint>(...);
FAnimBPExporter Exporter;
FString DSLCode = Exporter.ExportToAnimLang(AnimBP);

// Save to file
FFileHelper::SaveStringToFile(DSLCode, TEXT("MyAnimBP.animlang"));
```

### Import DSL to Animation Blueprint

```cpp
// C++ API
#include "AnimBPImporter.h"

FString DSLCode;
FFileHelper::LoadFileToString(DSLCode, TEXT("MyAnimBP.animlang"));

FAnimBPImporter Importer;
UAnimBlueprint* AnimBP = Importer.ImportFromAnimLang(DSLCode);
```

### Editor Integration

1. Right-click Animation Blueprint → **Export to AnimLang**
2. Edit `.animlang` file in text editor
3. Right-click `.animlang` file → **Import to Animation Blueprint**

---

## 📐 DSL Example

```lisp
;; Simple blend example
(anim-blueprint "ThirdPersonCharacter"
  :inputs [(float :speed 0.0 :range [0.0 600.0])]
  
  :output
    (blend (/ :speed 600.0)
      (sequence-player "Idle_Rifle" :loop true)
      (sequence-player "Run_Fwd_Rifle" :loop true)))
```

```lisp
;; State machine example
(anim-blueprint "Character"
  :state-machine :locomotion
    :states
      [(state :idle (sequence-player "Idle"))
       (state :walk (sequence-player "Walk"))]
    :transitions
      [(idle -> walk :condition (> :speed 10.0) :blend-time 0.2)
       (walk -> idle :condition (< :speed 10.0) :blend-time 0.3)])
```

See `Extras/DSL/Examples/` for more examples.

---

## 🏗️ Project Structure

```
AnimBP2FP/                       # Plugin root (THIS IS A STANDARD UE PLUGIN)
├── AnimBP2FP.uplugin            # Plugin descriptor (in root!)
├── Source/                      # Plugin source code
│   ├── AnimBP2FP/               # Runtime module
│   │   ├── Public/
│   │   │   ├── AnimBPExporter.h
│   │   │   ├── AnimBPImporter.h
│   │   │   ├── AnimLangAST.h
│   │   │   └── AnimNodeExporter.h
│   │   └── Private/
│   │       └── AnimNodeExporter.cpp
│   └── AnimBP2FPEditor/         # Editor module
│       ├── Public/
│       │   ├── AnimBP2FPEditorModule.h
│       │   └── AnimBP2FPSettings.h
│       └── Private/
│           ├── AnimBP2FPEditorModule.cpp
│           └── AnimBP2FPSettings.cpp
├── Content/                     # Plugin content
│   └── Icons/
├── Resources/                   # Plugin resources
├── Docs/                        # Documentation (part of plugin repo)
│   ├── README.md                # Full documentation
│   ├── PROGRESS.md              # Development progress
│   ├── PROJECT_SUMMARY.md       # Project summary
│   ├── PROJECT_INDEPENDENCE_DECISION.md
│   └── Research/                # Research documents
│       └── AnimBlueprintAnalysis.md
└── Extras/                      # Extra tools (not loaded by UE)
    ├── Tools/                   # Racket tooling (linter, formatter)
    │   ├── animlang-types.rkt
    │   ├── animlang-nodes.rkt
    │   ├── animlang-lint.rkt
    │   └── animlang-format.rkt
    ├── Tests/                   # Test cases
    │   ├── TestStrategy.md
    │   ├── UnitTests/
    │   ├── IntegrationTests/
    │   └── TestAssets/
    └── DSL/                     # DSL examples and specs
        └── Examples/
            ├── simple_blend.animlang
            ├── state_machine.animlang
            └── third_person_char.animlang
```

**Note**: `Extras/` is part of the repo but NOT part of the UE plugin. Use for development/testing only.

---

## 🔧 Configuration

### Project Settings

Edit → Project Settings → Plugins → AnimBP2FP:

- **Auto Export on Save**: Automatically export AnimBP to DSL when saved
- **Auto Generate Stubs**: Generate type definitions on editor startup (like `unreal.py`)
- **Export Path**: Where to save exported DSL files (default: `Intermediate/AnimLangStub/`)

### Editor Settings

Tools → AnimBP2FP:
- **Export Animation Blueprint**: Export selected AnimBP
- **Import DSL File**: Import `.animlang` file
- **Regenerate Type Stubs**: Manually trigger stub generation

---

## 🛠️ Dependencies

### Required
- Unreal Engine 5.6+
- C++20 compiler (MSVC 2022, Clang)

### Optional (for Extras tools)
- Racket 8.0+ (for linting/formatting)
- sexpp library (for S-expression parsing, included)

---

## 📚 Documentation

- **Full Documentation**: `Docs/README.md`
- **Development Progress**: `Docs/PROGRESS.md`
- **Project Summary**: `Docs/PROJECT_SUMMARY.md`
- **Research Papers**: `Docs/Research/`
- **DSL Specification**: `Extras/DSL/`
- **Test Strategy**: `Extras/Tests/TestStrategy.md`

---

## 🤝 Related Projects

- **MaterialBP2FP**: Sister project for Material Blueprints (coming soon)
  - Higher functional purity (~95% vs ~70%)
  - HLSL type system vs UE types
  - Independent tooling

See `Docs/PROJECT_INDEPENDENCE_DECISION.md` for why projects are separate.

---

## 🧪 Testing

```bash
# Run unit tests (requires UE Editor)
cd Extras/Tests
./RunTests.bat

# Run integration tests
./RunIntegrationTests.bat
```

---

## 🐛 Known Issues

See `Docs/PROGRESS.md` for current status and known issues.

**Current Status**: Phase 2 - DSL Parser (30% complete)

---

## 📖 License

MIT License (see LICENSE file)

---

## 🙋 Support

- **Issues**: GitHub Issues
- **Discussions**: GitHub Discussions
- **Repository**: https://github.com/cc8887/AnimationBP2FP-N

---

## 🎯 Roadmap

| Phase | Status | ETA |
|-------|--------|-----|
| Phase 1: Research & Design | ✅ Done | 2026-03-23 |
| Phase 2: DSL Parser | 🔄 10% | 2026-03-30 |
| Phase 3: Blueprint → DSL | ⏳ 0% | 2026-04-06 |
| Phase 4: DSL → Blueprint | ⏳ 0% | 2026-04-13 |
| Phase 5: Testing | ⏳ 0% | 2026-04-27 |
| Phase 6: Release | ⏳ 0% | 2026-05-11 |

---

**Version**: 0.1.0-alpha  
**Last Updated**: 2026-03-24  
**UE Version**: 5.6+  
**Functional Purity**: ~70% (state machines have effects)