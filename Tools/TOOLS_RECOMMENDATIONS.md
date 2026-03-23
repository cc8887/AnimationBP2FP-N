# AnimBP2FP - 现成工具与库推荐

## 概述

AnimBP2FP 使用 S-expression 语法，可以直接利用成熟的 Lisp/Scheme 工具链。本文档整理了可用的解析器、Linter、格式化工具和 AST 操作库。

---

## 🔧 C++ S-Expression 解析器库

### 1. **sexpp** ⭐️ 推荐
- **GitHub**: https://github.com/rnpgp/sexpp
- **描述**: MIT 的 Ronald Rivest 教授开发的 C++ S-expression 库
- **特性**:
  - 高性能、轻量级
  - 支持规范化（Canonical）和高级（Advanced）格式
  - 完整的解析和序列化支持
  - 经过密码学应用验证（RNP 项目使用）
- **许可**: MIT/BSD
- **集成难度**: ⭐️⭐️ 简单

**使用示例**：
```cpp
#include <sexp/sexp.h>

// 解析
sexp::sexp_input_stream is(input_string);
sexp::sexp_object obj;
is >> obj;

// 生成
sexp::sexp_output_stream os;
os << obj;
```

**为何推荐**：
- MIT 授权，商业友好
- 已在生产环境（RNP 密码学库）使用
- API 简洁，文档完善

---

### 2. **sexpr** (轻量级)
- **来源**: CSDN 开源库
- **描述**: 面向嵌入式和系统级编程的轻量级 S-expression 库
- **特性**:
  - 单头文件
  - 无外部依赖
  - 适合嵌入到 UE 插件
- **许可**: BSD-like
- **集成难度**: ⭐️ 最简单

**适用场景**：
- 只需要基础解析功能
- 希望减少依赖
- 需要完全控制代码

---

### 3. **c--_lisp_sexp_parser**
- **GitHub**: https://github.com/ajuc/c--_lisp_sexp_parser
- **描述**: 简单的 C++ Lisp S-expression 解析器
- **特性**:
  - 只有 2 个文件（lispparser.h/cpp）
  - 作为 XML 解析的替代品设计
- **许可**: MIT
- **集成难度**: ⭐️ 最简单

**注意**：
- 功能较基础，可能需要自己扩展

---

## 🎨 Pretty Printer / 格式化工具

### 1. **Racket/Scheme Pretty Print** ⭐️ 推荐
- **工具**: Racket 的 `pretty-print` 模块
- **用途**: 验证 DSL 输出格式
- **使用方式**:
  ```bash
  # 安装 Racket
  brew install racket  # macOS
  
  # 格式化文件
  racket -e "(require racket/pretty) (pretty-print (read))" < input.animlang
  ```

**集成到项目**：
```bash
# 作为格式化工具
make format:
    find DSL/ -name "*.animlang" | xargs racket-format.rkt
```

---

### 2. **sexp_pretty** (OCaml/Jane Street)
- **GitHub**: https://github.com/janestreet/sexp_pretty
- **描述**: Jane Street 的 S-expression 漂亮打印库
- **特性**:
  - 智能缩进
  - 可配置列宽
  - 生产级质量（Jane Street 内部使用）
- **语言**: OCaml（可通过 FFI 调用）

**适用场景**：
- 如果团队熟悉 OCaml
- 需要企业级格式化质量

---

## 🔍 Linter / 验证工具

### 推荐方案：基于 Racket 的自定义 Linter

由于 AnimLang 是专用 DSL，现成的 Lisp Linter 不能直接用。推荐方案：

#### 方案 A：Racket + 自定义规则 ⭐️ 推荐

**创建 AnimLang Linter**：

```racket
;; animlang-lint.rkt
#lang racket

(require racket/match)

(define (lint-animlang sexp)
  (match sexp
    [`(anim-blueprint ,name . ,rest)
     (check-blueprint-structure rest)]
    [_ (error "Invalid root form")]))

(define (check-blueprint-structure forms)
  (for ([form forms])
    (match form
      [`(:variables . ,vars)
       (check-variables vars)]
      [`(:anim-graph . ,graph)
       (check-anim-graph graph)]
      [_ (void)])))

(define (check-variables vars)
  (for ([var vars])
    (match var
      [`(,type ,name ,default . ,opts)
       (unless (member type '(float int bool vector))
         (error "Invalid type: ~a" type))]
      [_ (error "Invalid variable definition")])))

;; 使用
(define input (read))
(lint-animlang input)
```

**优势**：
- 完全控制验证规则
- 类型检查
- 语义验证
- 易于扩展

---

#### 方案 B：基于 sexpp + 自定义 C++ 验证器

```cpp
// AnimLangValidator.h
class FAnimLangValidator
{
public:
    struct FValidationError
    {
        FString Message;
        int32 Line;
        int32 Column;
    };
    
    static TArray<FValidationError> Validate(const FString& DSLCode);
    
private:
    static void ValidateBlueprint(const sexp::sexp_object& Root);
    static void ValidateVariables(const sexp::sexp_list& Vars);
    static void ValidateAnimGraph(const sexp::sexp_object& Graph);
    static void ValidateStateMachine(const sexp::sexp_object& SM);
};
```

---

## 🌳 AST 操作工具

### 1. Tree-sitter ⭐️ 现代化推荐

- **官网**: https://tree-sitter.github.io/tree-sitter/
- **描述**: 通用增量解析库，用于代码编辑器
- **特性**:
  - 增量解析（编辑器友好）
  - 语法高亮支持
  - LSP（Language Server Protocol）集成
  - 支持自定义语法

**为 AnimLang 创建 Tree-sitter 语法**：

```javascript
// grammar.js
module.exports = grammar({
  name: 'animlang',
  
  rules: {
    source_file: $ => $.sexp,
    
    sexp: $ => choice(
      $.list,
      $.atom
    ),
    
    list: $ => seq(
      '(',
      repeat($.sexp),
      ')'
    ),
    
    atom: $ => choice(
      $.symbol,
      $.number,
      $.string,
      $.keyword
    ),
    
    keyword: $ => /:[a-z-]+/,
    symbol: $ => /[a-z][a-z0-9-]*/,
    number: $ => /-?[0-9]+(\.[0-9]+)?/,
    string: $ => /"[^"]*"/
  }
});
```

**集成到 VSCode/Cursor**：
- 语法高亮
- 括号匹配
- 自动补全
- 错误检查

---

### 2. ANTLR4（备选）

如果需要更强大的解析能力：

```antlr
grammar AnimLang;

animBlueprint
    : '(' 'anim-blueprint' STRING variableDef* animGraph ')'
    ;

variableDef
    : '(' ':variables' variable* ')'
    ;

variable
    : '(' type KEYWORD defaultValue range? ')'
    ;

type
    : 'float' | 'int' | 'bool' | 'vector'
    ;
```

**优势**：
- 生成多种语言的解析器
- 完整的错误恢复
- 可视化工具

**劣势**：
- 依赖较重
- 对 S-expression 来说有点过度设计

---

## 📦 推荐技术栈

### Phase 2 实现方案

#### 方案 A：纯 C++（推荐用于 UE 插件）

```
sexpp (S-expression 解析)
    ↓
自定义 AST 构建器
    ↓
FTypeChecker (C++)
    ↓
UAnimBlueprint
```

**优势**：
- 无外部依赖
- 性能最佳
- UE 集成简单

**工具链**：
- 解析器：**sexpp**
- Linter：自定义 C++ 验证器
- 格式化：Racket（开发阶段）

---

#### 方案 B：混合方案（推荐用于开发工具）

```
Racket (DSL 验证和格式化)
    ↓
sexpp (C++ 解析)
    ↓
Tree-sitter (编辑器集成)
    ↓
UAnimBlueprint
```

**优势**：
- 开发效率高
- 工具链完善
- 可独立使用各部分

**工具链**：
- 解析器：**sexpp** (生产) + **Racket** (开发)
- Linter：**Racket 脚本**
- 格式化：**Racket pretty-print**
- 编辑器：**Tree-sitter**

---

## 🎯 具体实施建议

### 第一周（Phase 2.1）

1. **集成 sexpp**
   ```bash
   git submodule add https://github.com/rnpgp/sexpp ThirdParty/sexpp
   ```

2. **编写基础解析器**
   ```cpp
   FString DSLCode = LoadFile("example.animlang");
   sexp::sexp_input_stream is(TCHAR_TO_UTF8(*DSLCode));
   sexp::sexp_object obj;
   is >> obj;
   ```

3. **创建 Racket Linter**
   ```bash
   cd AnimBP2FP/Tools
   touch animlang-lint.rkt
   chmod +x animlang-lint.rkt
   ```

### 第二周（Phase 2.2）

4. **实现类型检查器**
   - 基于 C++，集成到插件
   - 使用 sexpp 的 AST

5. **集成 Tree-sitter**（可选）
   - 为 VSCode/Cursor 提供语法支持

### 第三周（Phase 2.3）

6. **完善工具链**
   - 格式化脚本
   - CI/CD 集成
   - 单元测试

---

## 🔗 资源链接

### 库与工具
- **sexpp**: https://github.com/rnpgp/sexpp
- **Tree-sitter**: https://tree-sitter.github.io/
- **Racket**: https://racket-lang.org/
- **sexp_pretty**: https://github.com/janestreet/sexp_pretty

### 文档与教程
- MIT SEXP 规范: https://people.csail.mit.edu/rivest/Sexp.txt
- Tree-sitter 教程: https://tree-sitter.github.io/tree-sitter/creating-parsers
- Racket Guide: https://docs.racket-lang.org/guide/

---

## 📝 总结

### 核心推荐

| 用途 | 工具 | 理由 |
|------|------|------|
| **C++ 解析** | sexpp | MIT 教授开发，生产级质量 |
| **Linter** | Racket 脚本 | 灵活、易扩展 |
| **格式化** | Racket pretty-print | 现成可用 |
| **编辑器** | Tree-sitter | 现代化，LSP 集成 |

### 时间投入估算

- sexpp 集成：**1 天**
- Racket Linter：**2 天**
- Tree-sitter 语法：**1 天**（可选）
- 类型检查器：**2-3 天**

**总计**: Phase 2 可在 **5-7 天**内完成，比从零实现节省 **50% 时间**。

---

**最后更新**: 2026-03-23 20:30 GMT+8