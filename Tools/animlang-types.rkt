;; AnimLang Type Definitions (Stub File)
;; 类似于 TypeScript 的 .d.ts 或 Python 的 .pyi
;; 用于 IDE 自动补全、类型检查和文档生成
;;
;; 更新于 2026-03-25: 状态机完整展开 + define 拓扑排序
;;   - SaveCachedPose → (define Name body) 顶层绑定
;;   - UseCachedPose → (Name) 变量引用
;;   - define 按拓扑排序输出（依赖在前，被依赖在后）
;;   - 状态机完整展开：状态 + 动画子树 + 转换列表
;;   - Pin->DefaultObject 输出为 (asset "path") 格式
;;   - 所有参数使用 :key value 关键字风格
;;   - 动画数据输入使用 :pin-name (child ...) 关键字
;;   - 外部引用使用 (ref "Node Title") 表示
;;   - 节点名称使用 kebab-case

#lang typed/racket

;; ========== 基础类型 ==========

(define-type PinType
  (U 'Pose 'Float 'Int 'Bool 'Vector 'Rotator 'Transform 'Name 'Object 'Byte))

(define-type AnimSequence String)  ;; 资产名称（如 "Walk_Fwd_Rifle"）
(define-type BlendSpace String)    ;; BlendSpace 资产名称
(define-type BoneName String)      ;; 骨骼名称
(define-type SlotName String)      ;; Slot 名称
(define-type CacheName String)     ;; 缓存姿态名称
(define-type CurveName String)     ;; 曲线名称

;; ========== 值表达式类型 ==========

;; 参数值可以是字面量、外部引用或资产引用
(define-type ParamValue
  (U Float
     Integer
     Boolean
     String
     RefExpr      ;; (ref "Node Title")
     AssetRef))   ;; (asset "/Game/Path/To/Asset")

;; 外部引用 - 连接到蓝图逻辑节点（Get/Set/函数调用等）
;; 在导出格式中表示为: (ref "Get VariableName") 或 (ref "Some Function")
(struct RefExpr
  ([target : String])   ;; 被引用的蓝图节点标题
  #:transparent)

;; 资产引用 - Pin->DefaultObject 不为空时输出
;; 在导出格式中表示为: (asset "/Game/Path/To/Asset")
(struct AssetRef
  ([path : String])     ;; UE 资产路径
  #:transparent)

;; 通用表达式类型（包括算术/比较/逻辑）
(define-type Expr
  (U Float
     Integer
     Boolean
     String
     Symbol        ;; :parameter-reference
     RefExpr       ;; (ref "Node Title")
     (List '+ Expr Expr)
     (List '- Expr Expr)
     (List '* Expr Expr)
     (List '/ Expr Expr)
     (List '> Expr Expr)
     (List '< Expr Expr)
     (List '>= Expr Expr)
     (List '<= Expr Expr)
     (List '= Expr Expr)
     (List 'and Expr Expr)
     (List 'or Expr Expr)
     (List 'not Expr)
     (List 'if Expr Expr Expr)))

;; ========== 动画节点类型 ==========

;; AnimNode 是所有动画节点的抽象类型
;; 每个动画节点都是一个 S-expression:
;;   (node-type :param1 value1 :param2 value2
;;     :pose-input-a (child-node ...)
;;     :pose-input-b (child-node ...))
(define-type AnimNode Any)

;; ========== 辅助结构 ==========

;; 状态机状态 (完整展开)
;; 每个状态包含名称和内部动画子树
;; 在导出格式中表示为 state-machine 的命名子节点：
;;   :state-name-kebab (animation-subtree ...)
;; 示例:
;;   :in-ragdoll
;;     (sequence-player :name "ALS_Flail" :loop true)
(struct State
  ([name : Symbol]             ;; 状态名称 (原始名，如 "In Ragdoll")
   [animation : AnimNode])     ;; 该状态的动画子树 (从 StateResult 遍历)
  #:transparent)

;; 状态转换
;; 在导出格式中表示为 :transitions 列表：
;;   (from -> to :duration 0.2 :priority 1 :rule (ref "条件节点"))
;;   (from -> to :duration 0.25 :priority 1 :rule (auto-rule :time-remaining 0.25))
;; 属性:
;;   :duration       Float    ;; 交叉淡入淡出时长
;;   :priority       Integer  ;; 优先级 (数字小的优先)
;;   :bidirectional  Boolean  ;; 双向转换 (仅 true 时输出)
;;   :rule           Expr     ;; 转换条件: (ref "...") 或 (auto-rule ...)
(struct Transition
  ([from-state : Symbol]       ;; 源状态名
   [to-state : Symbol]         ;; 目标状态名
   [condition : Expr]          ;; 转换条件 (ref 或 auto-rule)
   [duration : Float]          ;; 交叉淡入淡出时长
   [priority : Integer]        ;; 优先级
   [bidirectional : Boolean])  ;; 是否双向
  #:transparent)

;; 分层混合层
(struct BlendLayer
  ([name : String]
   [weight : (U Float RefExpr)]
   [blend-mode : (U 'blend 'replace)]
   [bone-filter : BoneName]
   [blend-depth : Integer]
   [source : AnimNode])
  #:transparent)

;; ========== 变量定义 ==========

;; 在导出格式中:
;;   (float :VariableName)
;;   (bool :VariableName)
;;   (int :VariableName)
(struct VariableDef
  ([type : (U 'float 'bool 'int 'byte 'name 'object)]
   [name : Symbol]
   [default : Any]
   [range : (Option (List Float Float))]
   [description : (Option String)])
  #:transparent)

;; ========== 定义绑定 ==========

;; (define Name body)
;; 将 SaveCachedPose 提升为顶层绑定，引用处用 (Name) 代替
;; 等价于 Scheme 的 (define name expr)
;;
;; 示例:
;;   (define Post-Layering
;;     (layered-bone-blend ...))
;;
;; 引用:
;;   (apply-mesh-space-additive :base (Post-Layering) ...)
(struct CachedPoseDef
  ([name : Symbol]        ;; kebab-case 标识符，如 Post-Layering
   [body : AnimNode])     ;; 绑定的子树
  #:transparent)

;; ========== 动画蓝图（顶层结构） ==========

;; 导出格式:
;;   (anim-blueprint "Name"
;;     :variables [ (float :Var1) (bool :Var2) ... ]
;;
;;     (define Binding-A (some-node ...))
;;     (define Binding-B (other-node ... (Binding-A) ...))
;;
;;     :anim-graph (root-node ... (Binding-B) ...))
(struct AnimBlueprint
  ([name : String]
   [variables : (Listof VariableDef)]
   [defines : (Listof CachedPoseDef)]  ;; (define ...) 绑定
   [anim-graph : AnimNode])
  #:transparent)

;; ========== 导出 ==========

(provide PinType
         AnimSequence
         BlendSpace
         BoneName
         SlotName
         CacheName
         CurveName
         ParamValue
         RefExpr
         AssetRef
         Expr
         State
         Transition
         BlendLayer
         VariableDef
         CachedPoseDef
         AnimBlueprint
         AnimNode)
