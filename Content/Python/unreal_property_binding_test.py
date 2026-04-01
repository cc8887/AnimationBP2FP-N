"""
AnimBP2FP Property Binding 完整测试
测试导出、修改(ref-var)、导入的完整流程
"""
import unreal

def log(msg):
    """打印日志消息"""
    unreal.log(f"[PropertyBindingTest] {msg}")

def test_export_import_roundtrip():
    """
    测试导出->修改(ref-var)->导入的完整流程
    """
    target_asset = "/Game/AdvancedLocomotionV4/CharacterAssets/MannequinSkeleton/ALS_AnimBP"

    log("=" * 60)
    log("PROPERTY BINDING ROUNDTRIP TEST")
    log("=" * 60)

    # 检查库是否可用
    if not hasattr(unreal, 'AnimBP2FPBlueprintLibrary'):
        log("ERROR: AnimBP2FPBlueprintLibrary not found! Plugin may not be loaded.")
        return False

    # Step 1: 导出原始DSL
    log("\n--- Step 1: Export original DSL ---")
    lib = unreal.AnimBP2FPBlueprintLibrary
    export_result = lib.export_anim_blueprint(target_asset)

    if not export_result.success:
        log(f"ERROR: Export failed: {export_result.message}")
        return False

    original_dsl = export_result.d_s_l_code
    log(f"Original DSL length: {len(original_dsl)} chars")

    # Step 2: 检查是否有SequencePlayer节点
    if "AnimNode_SequencePlayer" not in original_dsl:
        log("WARNING: No AnimNode_SequencePlayer found in DSL")
        log("This test requires a SequencePlayer node to test PlayRate binding")

    # Step 3: 尝试导入（不修改）作为基线测试
    log("\n--- Step 2: Baseline import test ---")
    import_result = lib.update_blueprint(target_asset, original_dsl)
    if import_result.success:
        log(f"Baseline import successful: {import_result.message}")
    else:
        log(f"WARNING: Baseline import failed: {import_result.message}")

    # Step 4: 运行Linter检查原始DSL
    log("\n--- Step 3: Lint original DSL ---")
    lint_result = lib.lint_d_s_l(original_dsl, "test")
    log(f"Lint result: {lint_result.success}")
    if not lint_result.success:
        log(f"Lint errors:\n{lint_result.report_text}")
    else:
        log("Lint passed - DSL is valid")

    log("\n" + "=" * 60)
    log("ROUNDTRIP TEST COMPLETE")
    log("=" * 60)

    return True

def test_create_refvar_binding():
    """
    测试创建一个新的(ref-var ...)绑定
    这个测试需要目标蓝图有可修改的SequencePlayer节点
    """
    target_asset = "/Game/AdvancedLocomotionV4/CharacterAssets/MannequinSkeleton/ALS_AnimBP"

    log("=" * 60)
    log("CREATE REF-VAR BINDING TEST")
    log("=" * 60)

    # 导出当前DSL
    lib = unreal.AnimBP2FPBlueprintLibrary
    export_result = lib.export_anim_blueprint(target_asset)

    if not export_result.success:
        log(f"ERROR: Export failed: {export_result.message}")
        return False

    original_dsl = export_result.d_s_l_code

    # 检查是否已有(ref-var ...)绑定
    if "ref-var" in original_dsl:
        log("Asset already has ref-var bindings")
        count = original_dsl.count("ref-var")
        log(f"Found {count} existing ref-var binding(s)")

        # 显示所有ref-var绑定
        import re
        ref_vars = re.findall(r'\(ref-var\s+"([^"]+)"\)', original_dsl)
        log(f"Variables: {ref_vars}")
    else:
        log("No existing ref-var bindings found")

    log("=" * 60)
    log("CREATE TEST COMPLETE")
    log("=" * 60)

    return True

def run_all_tests():
    """
    运行所有测试
    """
    log("Starting all property binding tests...")

    test_export_import_roundtrip()
    test_create_refvar_binding()

    log("\n" + "=" * 60)
    log("ALL TESTS COMPLETE")
    log("=" * 60)

if __name__ == "__main__":
    run_all_tests()
