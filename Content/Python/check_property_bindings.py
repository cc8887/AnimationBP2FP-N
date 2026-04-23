"""
检查AnimBlueprint资产的属性绑定
用于验证(ref-var ...)功能是否正常工作
"""
import unreal

def log(msg):
    """打印日志消息"""
    unreal.log(f"[PropertyBindingCheck] {msg}")

def get_node_property_bindings(node):
    """
    获取节点的属性绑定信息
    返回: dict {property_name: variable_name}
    """
    bindings = {}
    try:
        binding = node.get_editor_property("Binding")
        if binding:
            property_bindings = binding.get_editor_property("PropertyBindings")
            for prop_name, binding_info in property_bindings.items():
                is_bound = binding_info.get_editor_property("bIsBound")
                if is_bound:
                    property_path = binding_info.get_editor_property("PropertyPath")
                    if property_path and len(property_path) > 0:
                        bindings[str(prop_name)] = str(property_path[0])
    except Exception as e:
        log(f"Error reading bindings: {e}")
    return bindings

def check_blueprint_property_bindings(blueprint_path):
    """
    检查指定蓝图的所有属性绑定

    Args:
        blueprint_path: 蓝图路径，如 "/Game/AdvancedLocomotionV4/CharacterAssets/MannequinSkeleton/Editor"

    Returns:
        dict: 包含所有节点及其绑定的信息
    """
    log(f"Loading asset: {blueprint_path}")

    # 加载蓝图
    blueprint = unreal.load_asset(blueprint_path)
    if not blueprint:
        log(f"ERROR: Failed to load asset at {blueprint_path}")
        return None

    # 检查是否是AnimBlueprint
    class_name = blueprint.get_class().get_name()
    log(f"Loaded asset: {blueprint.get_name()} (Class: {class_name})")

    if class_name != "AnimBlueprint":
        log(f"WARNING: Asset is not an AnimBlueprint (it's a {class_name})")
        log("This asset cannot be processed by AnimBP2FP")
        return None

    # 获取动画图
    anim_graph = blueprint.anim_graph
    if not anim_graph:
        log("No anim_graph found on blueprint")
        return None

    # 获取图中的所有节点
    nodes = anim_graph.nodes
    log(f"Found {len(nodes)} nodes in anim graph")

    results = {
        "blueprint": blueprint_path,
        "total_nodes": len(nodes),
        "nodes_with_bindings": 0,
        "bindings": []
    }

    for node in nodes:
        node_name = node.get_name()
        node_class = node.get_class().get_name()

        # 只检查AnimGraphNode类型的节点
        if "AnimGraphNode" in node_class:
            bindings = get_node_property_bindings(node)
            if bindings:
                results["nodes_with_bindings"] += 1
                node_info = {
                    "node_name": node_name,
                    "node_class": node_class,
                    "bindings": bindings
                }
                results["bindings"].append(node_info)
                log(f"  Node: {node_name} ({node_class})")
                for prop_name, var_name in bindings.items():
                    log(f"    {prop_name} -> {var_name}")

    log(f"Found {results['nodes_with_bindings']} nodes with property bindings")
    return results

def export_and_check_dsl(blueprint_path):
    """
    导出蓝图到DSL并检查是否包含(ref-var ...)语法

    Args:
        blueprint_path: 蓝图路径
    """
    log(f"Exporting blueprint to DSL: {blueprint_path}")

    # 检查AnimBP2FPBlueprintLibrary是否可用
    if not hasattr(unreal, 'AnimBP2FPBlueprintLibrary'):
        log("ERROR: AnimBP2FPBlueprintLibrary not found!")
        log("Please ensure:")
        log("  1. AnimBP2FP plugin is compiled")
        log("  2. AnimBP2FP plugin is enabled in Edit > Plugins")
        log("  3. Project has been restarted after plugin changes")
        return None

    # 使用AnimBP2FPBlueprintLibrary导出
    lib = unreal.AnimBP2FPBlueprintLibrary
    result = lib.export_anim_blueprint(blueprint_path)

    if result.success:
        log("Export successful!")
        log(f"DSL code length: {len(result.d_s_l_code)} chars")

        # 检查是否包含ref-var语法
        if "ref-var" in result.d_s_l_code:
            log("SUCCESS: DSL contains (ref-var ...) syntax!")
            # 统计ref-var出现次数
            count = result.d_s_l_code.count("ref-var")
            log(f"Found {count} ref-var binding(s)")
        else:
            log("WARNING: DSL does not contain (ref-var ...) syntax")

        # 打印前2000字符用于检查
        log("DSL Preview (first 2000 chars):")
        log("=" * 50)
        log(result.d_s_l_code[:2000])
        log("=" * 50)

        return result.d_s_l_code
    else:
        log(f"Export failed: {result.message}")
        return None

def run_full_test(asset_path=None):
    """
    运行完整测试：检查属性绑定并导出DSL

    Args:
        asset_path: 可选，指定要测试的资产路径。默认为ALS_AnimBP
    """
    target_asset = asset_path or "/Game/AdvancedLocomotionV4/CharacterAssets/MannequinSkeleton/ALS_AnimBP"

    log("=" * 60)
    log("PROPERTY BINDING CHECK TEST")
    log("=" * 60)

    # Step 1: 检查现有属性绑定
    log("\n--- Step 1: Checking existing property bindings ---")
    binding_info = check_blueprint_property_bindings(target_asset)

    # Step 2: 导出DSL并检查语法
    log("\n--- Step 2: Exporting to DSL ---")
    dsl_code = export_and_check_dsl(target_asset)

    log("\n" + "=" * 60)
    log("TEST COMPLETE")
    log("=" * 60)

    return binding_info, dsl_code

if __name__ == "__main__":
    run_full_test()
