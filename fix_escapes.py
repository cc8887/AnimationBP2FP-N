import glob

# 扫描所有需要修复的文件
patterns = [
    'G:/Github/AdvancedLocomotionSystemV/Plugins/AnimBP2FP/Source/**/*.h',
    'G:/Github/AdvancedLocomotionSystemV/Plugins/AnimBP2FP/Source/**/*.cpp',
    'G:/Github/AdvancedLocomotionSystemV/Plugins/AnimBP2FP/Source/**/*.cs',
]

fixed_files = []
for pattern in patterns:
    for filepath in glob.glob(pattern, recursive=True):
        with open(filepath, 'r', encoding='utf-8', errors='ignore') as f:
            content = f.read()
        if '\\"' in content:
            fixed = content.replace('\\"', '"')
            with open(filepath, 'w', encoding='utf-8', newline='') as f:
                f.write(fixed)
            fixed_files.append(filepath)
            print('Fixed: ' + filepath)

print(f'\nTotal fixed: {len(fixed_files)} files')

filepath = 'G:/Github/AdvancedLocomotionSystemV/Plugins/AnimBP2FP/Source/AnimBP2FP/Public/AnimBPExporter.h'
with open(filepath, 'rb') as f:
    content = f.read(200)
print('Raw bytes:', repr(content))
print()
# 检查是否有 backslash + quote (0x5c 0x22)
backslash_quote = bytes([0x5c, 0x22])
print('Contains backslash+quote:', backslash_quote in content)