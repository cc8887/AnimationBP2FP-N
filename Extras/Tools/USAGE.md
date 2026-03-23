# AnimBP2FP 工具使用指南

## 安装依赖

### 1. 安装 Racket（用于 Linter 和格式化）

```bash
# macOS
brew install racket

# Ubuntu/Debian
sudo apt-get install racket

# Windows
# 下载安装器: https://racket-lang.org/
```

### 2. 验证安装

```bash
racket --version
# 应输出: Welcome to Racket v8.x
```

---

## 工具使用

### 🔍 Linter - 验证 DSL 语法

```bash
# 给脚本添加执行权限
chmod +x Tools/animlang-lint.rkt

# 验证单个文件
./Tools/animlang-lint.rkt DSL/Examples/simple_blend.animlang

# 验证所有示例
find DSL/Examples -name \"*.animlang\" -exec ./Tools/animlang-lint.rkt {} \;
```

**输出示例**：
```
✓ No errors found
```

或

```
✗ Found 2 error(s):
  - Blueprint name must be a string
  - Invalid variable type: float2
```

---

### 🎨 Formatter - 格式化 DSL 代码

```bash
# 给脚本添加执行权限
chmod +x Tools/animlang-format.rkt

# 格式化到标准输出
./Tools/animlang-format.rkt DSL/Examples/simple_blend.animlang

# 格式化并覆盖原文件
./Tools/animlang-format.rkt DSL/Examples/simple_blend.animlang > temp.animlang
mv temp.animlang DSL/Examples/simple_blend.animlang
```

**格式化前**：
```lisp
(anim-blueprint \"Test\" :variables [(float :speed 0.0)] :anim-graph (blend 0.5 (sequence-player \"A\") (sequence-player \"B\")))
```

**格式化后**：
```lisp
(anim-blueprint \"Test\"
  :variables [(float :speed 0.0)]
  :anim-graph
    (blend 0.5
      (sequence-player \"A\")
      (sequence-player \"B\")))
```

---

## 集成到 Git Hooks

### Pre-commit Hook

在 `.git/hooks/pre-commit` 中添加：

```bash
#!/bin/bash

# 验证所有 .animlang 文件
for file in $(git diff --cached --name-only --diff-filter=ACM | grep '\.animlang$'); do
    echo \"Linting $file...\"
    ./Tools/animlang-lint.rkt \"$file\"
    if [ $? -ne 0 ]; then
        echo \"Lint failed for $file\"
        exit 1
    fi
done

echo \"All AnimLang files are valid ✓\"
```

给 hook 添加执行权限：
```bash
chmod +x .git/hooks/pre-commit
```

---

## CI/CD 集成

### GitHub Actions

创建 `.github/workflows/lint.yml`：

```yaml
name: AnimLang Lint

on: [push, pull_request]

jobs:
  lint:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v3
      
      - name: Install Racket
        run: |
          sudo apt-get update
          sudo apt-get install -y racket
      
      - name: Lint AnimLang files
        run: |
          find DSL/Examples -name \"*.animlang\" -exec ./Tools/animlang-lint.rkt {} \;
```

---

## VSCode 集成（可选）

### 语法高亮

创建 `.vscode/extensions.json`：

```json
{
  \"recommendations\": [
    \"betterthantomorrow.calva\",  // Lisp/Scheme 语法高亮
    \"draivin.hscopes\"            // 额外的语法支持
  ]
}
```

### 自定义语言

创建 `.vscode/settings.json`：

```json
{
  \"files.associations\": {
    \"*.animlang\": \"lisp\"
  }
}
```

---

## 下一步

1. ✅ 安装 Racket
2. ✅ 测试 Linter
3. ⏳ 集成 sexpp C++ 库（Phase 2）
4. ⏳ 实现类型检查器（Phase 2）

---

**更新时间**: 2026-03-23 20:35 GMT+8