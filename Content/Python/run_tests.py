"""
运行所有AnimBP2FP Property Binding测试
在UE Python控制台中执行: import run_tests
"""
import check_property_bindings
import unreal_property_binding_test

def run_all():
    """运行所有测试"""
    print("=" * 60)
    print("AnimBP2FP Property Binding Test Suite")
    print("=" * 60)

    # 运行属性绑定检查
    print("\n>>> Test 1: Check Property Bindings <<<")
    check_property_bindings.run_full_test()

    # 运行导入导出测试
    print("\n>>> Test 2: Import/Export Roundtrip <<<")
    unreal_property_binding_test.run_all_tests()

    print("\n" + "=" * 60)
    print("All tests completed!")
    print("=" * 60)

if __name__ == "__main__":
    run_all()
