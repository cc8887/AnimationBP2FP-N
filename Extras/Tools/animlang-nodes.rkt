;; AnimLang Node Library (类型存根)
;; 基于 UE5.6 动画系统 + AnimBP2FP 导出格式
;;
;; 更新于 2026-03-25: define 绑定 + 变量引用
;;   - SaveCachedPose 提升为顶层 (define Name body)
;;   - UseCachedPose 退化为变量引用 (Name)
;;   - 所有节点使用 (node-type :key value ... :pose-input (child ...)) 格式
;;   - 参数来自节点的 Input Pin（自动提取）
;;   - Pose 输入以 pin 名称作为关键字标识
;;   - 外部连接使用 (ref "Node Title") 表示
;;
;; 每个节点定义说明：
;;   ;; 参数 (非 Pose 类型 Input Pin):
;;     :param-name  Type  ;; 描述 - [default: 值] 或 [连接]
;;   ;; Pose 输入 (Struct 类型 Input Pin):
;;     :pin-name    AnimNode  ;; 描述

#lang typed/racket

(require "animlang-types.rkt")

;; ============================================================
;;  基础动画播放
;; ============================================================

;; 序列播放器 - 播放单个动画序列
;; UE: UAnimGraphNode_SequencePlayer
;; 导出示例:
;;   (sequence-player :name "Walk_Fwd_Rifle" :loop true)
;;
;; 参数:
;;   :name          String      ;; 动画序列资产名称 (从 Node.GetSequence() 提取)
;;   :loop          Boolean     ;; 是否循环 (从 Node.IsLooping() 提取)
;;   :play-rate     Float       ;; 播放速率 (仅非 1.0 时输出)
;;   + 自动提取的其他 pin 参数
;; Pose 输入: 无 (叶子节点)
(: sequence-player (->* ()
                        (#:name String
                         #:loop Boolean
                         #:play-rate ParamValue)
                        AnimNode))

;; 序列求值器 - 在指定时间点采样动画
;; UE: UAnimGraphNode_SequenceEvaluator
;; 导出示例:
;;   (sequence-evaluator :explicit-time (ref "Get Draw"))
;;   (sequence-evaluator :explicit-time 0.000000)
;;
;; 参数:
;;   :explicit-time  ParamValue  ;; 采样时间点 [可连接]
;;   + 自动提取的其他 pin 参数
;; Pose 输入: 无 (叶子节点)
(: sequence-evaluator (->* ()
                           (#:explicit-time ParamValue)
                           AnimNode))

;; 混合空间播放器 (1D/2D)
;; UE: UAnimGraphNode_BlendSpacePlayer
;; 导出示例:
;;   (blendspace-player :name "NewBlendSpace1D" :loop true :x (ref "Get Speed"))
;;
;; 参数:
;;   :name        String      ;; BlendSpace 资产名称 (从 Node.GetBlendSpace() 提取)
;;   :loop        Boolean     ;; 是否循环
;;   :play-rate   Float       ;; 播放速率 (仅非 1.0 时输出)
;;   :x           ParamValue  ;; X 轴输入 [可连接]
;;   :y           ParamValue  ;; Y 轴输入 [可连接]
;;   + 自动提取的其他 pin 参数
;; Pose 输入: 无 (叶子节点)
(: blendspace-player (->* ()
                          (#:name String
                           #:loop Boolean
                           #:play-rate ParamValue
                           #:x ParamValue
                           #:y ParamValue)
                          AnimNode))

;; ============================================================
;;  混合节点
;; ============================================================

;; 二路混合 (TwoWayBlend) - 按 Alpha 混合两个姿态
;; UE: UAnimGraphNode_TwoWayBlend
;; 导出示例:
;;   (blend :alpha-curve-name "Enable_SpineRotation"
;;     :a (child-a ...)
;;     :b (child-b ...))
;;
;; 参数 (自动提取的 pin):
;;   :alpha             ParamValue  ;; 混合权重 0~1 [可连接]
;;   :alpha-curve-name  String      ;; Alpha 曲线名称
;;   + 其他 pin 参数
;; Pose 输入:
;;   :a    AnimNode  ;; 第一个姿态 (Alpha=0 时完全输出)
;;   :b    AnimNode  ;; 第二个姿态 (Alpha=1 时完全输出)
(: blend (->* ()
              (#:alpha ParamValue
               #:alpha-curve-name String
               #:a AnimNode
               #:b AnimNode)
              AnimNode))

;; 混合列表 - 按 Bool/Int/Enum 值选择姿态
;; UE: UAnimGraphNode_BlendListByBool / BlendListByInt / BlendListByEnum
;; 导出示例:
;;   (blend-list :class "AnimGraphNode_BlendListByEnum"
;;               :blend-time_0 0.400000 :blend-time_1 0.500000
;;               :active-enum-value (ref "Get MovementState")
;;     :blend-pose_0 (child-0 ...)
;;     :blend-pose_1 (child-1 ...))
;;
;; 参数:
;;   :class               String      ;; UE 类名 (区分 ByBool/ByInt/ByEnum)
;;   :active-enum-value   ParamValue  ;; 选择器值 (ByEnum) [可连接]
;;   :active-child-index  ParamValue  ;; 选择器值 (ByInt)
;;   :b-active-value      ParamValue  ;; 选择器值 (ByBool) [可连接]
;;   :blend-time_N        Float       ;; 每个输入的混合时间
;;   + 其他 pin 参数
;; Pose 输入 (动态数量):
;;   :blend-pose_0   AnimNode  ;; 第 0 个姿态
;;   :blend-pose_1   AnimNode  ;; 第 1 个姿态
;;   :blend-pose_N   AnimNode  ;; 第 N 个姿态...
(: blend-list (->* ()
                   (#:class String
                    #:active-enum-value ParamValue
                    #:active-child-index ParamValue
                    #:b-active-value ParamValue
                    ;; :blend-time_N 和 :blend-pose_N 是动态的
                    )
                   AnimNode))

;; 叠加混合 (ApplyAdditive)
;; UE: UAnimGraphNode_ApplyAdditive
;; 导出示例:
;;   (apply-additive :alpha (ref "Get Enable_AimOffset")
;;     :base (base-child ...)
;;     :additive (additive-child ...))
;;
;; 参数 (自动提取):
;;   :alpha           ParamValue  ;; 叠加权重 [可连接]
;;   :lod-threshold   Integer     ;; LOD 阈值
;;   + 其他 pin 参数
;; Pose 输入:
;;   :base       AnimNode  ;; 基础姿态
;;   :additive   AnimNode  ;; 叠加姿态
(: apply-additive (->* ()
                       (#:alpha ParamValue
                        #:lod-threshold Integer
                        #:base AnimNode
                        #:additive AnimNode)
                       AnimNode))

;; Mesh Space 叠加混合
;; UE: UAnimGraphNode_ApplyMeshSpaceAdditive
;; 导出示例:
;;   (apply-mesh-space-additive :alpha (ref "Get Enable_AimOffset")
;;     :base (base-child ...)
;;     :additive (additive-child ...))
;;
;; 参数与 Pose 输入同 apply-additive
(: apply-mesh-space-additive (->* ()
                                  (#:alpha ParamValue
                                   #:base AnimNode
                                   #:additive AnimNode)
                                  AnimNode))

;; 分层骨骼混合
;; UE: UAnimGraphNode_LayeredBoneBlend
;; 导出示例:
;;   (layered-bone-blend :blend-weights_0 1.000000
;;     :base-pose (base ...)
;;     :blend-poses_0 (layer-0 ...))
;;
;; 参数 (自动提取):
;;   :blend-weights_N  Float  ;; 每层的混合权重
;;   + 其他 pin 参数
;; Pose 输入 (动态数量):
;;   :base-pose      AnimNode  ;; 基础姿态
;;   :blend-poses_0  AnimNode  ;; 叠加层 0
;;   :blend-poses_N  AnimNode  ;; 叠加层 N...
(: layered-bone-blend (->* ()
                            (#:base-pose AnimNode
                             ;; :blend-weights_N 和 :blend-poses_N 是动态的
                             )
                            AnimNode))

;; ============================================================
;;  状态机
;; ============================================================

;; 状态机 (当前仅输出名称，详细状态/转换待完善)
;; UE: UAnimGraphNode_StateMachine
;; 导出示例:
;;   (state-machine :name "Ragdoll States")
;;
;; 参数:
;;   :name  String  ;; 状态机名称
;; Pose 输入: 无 (内部管理)
(: state-machine (->* ()
                      (#:name String)
                      AnimNode))

;; ============================================================
;;  定义绑定 (Cached Pose → define / 变量引用)
;; ============================================================

;; SaveCachedPose 提升为顶层 (define Name body)
;; UseCachedPose 退化为裸变量引用 (Name)
;;
;; UE: UAnimGraphNode_SaveCachedPose → (define Name body)
;; UE: UAnimGraphNode_UseCachedPose → (Name)
;;
;; 这是 Lisp 的 define 语义:
;;   (define Post-Layering          ;; 命名绑定
;;     (layered-bone-blend ...))    ;; 子树
;;
;;   (apply-mesh-space-additive
;;     :base (Post-Layering)        ;; 引用处直接用变量名
;;     ...)
;;
;; 名称规则: 空格 → 连字符 ("Post Layering" → "Post-Layering")
;;
;; define 出现在 anim-blueprint 的 :variables 和 :anim-graph 之间
;; 同一 define 可以被多处引用
;;
;; 注意: save-cached-pose 和 use-cached-pose 不再作为独立节点输出

;; ============================================================
;;  骨骼修改器
;; ============================================================

;; 两骨骼 IK
;; UE: UAnimGraphNode_TwoBoneIK
;; 导出示例:
;;   (two-bone-i-k :alpha (ref "Get Enable_HandIK_R")
;;     :component-pose (child ...))
;;
;; 参数 (自动提取):
;;   :alpha               ParamValue  ;; IK 强度 [可连接]
;;   :effector-location   ParamValue  ;; 效果器位置
;;   :joint-target        ParamValue  ;; 关节目标
;;   + 其他 pin 参数
;; Pose 输入:
;;   :component-pose  AnimNode  ;; 输入姿态 (Component Space)
(: two-bone-i-k (->* ()
                     (#:alpha ParamValue
                      #:component-pose AnimNode)
                     AnimNode))

;; 骨骼修改（变换）
;; UE: UAnimGraphNode_ModifyBone
;; 导出示例:
;;   (modify-bone
;;     :component-pose (child ...))
;;
;; 参数 (自动提取):
;;   :translation       ParamValue  ;; 位移
;;   :rotation          ParamValue  ;; 旋转
;;   :scale             ParamValue  ;; 缩放
;;   + 其他 pin 参数
;; Pose 输入:
;;   :component-pose  AnimNode  ;; 输入姿态 (Component Space)
(: modify-bone (->* ()
                    (#:translation ParamValue
                     #:rotation ParamValue
                     #:scale ParamValue
                     #:component-pose AnimNode)
                    AnimNode))

;; 曲线修改
;; UE: UAnimGraphNode_ModifyCurve
;; 导出示例:
;;   (modify-curve :curve-values_0 1.000000
;;     :source-pose (child ...))
;;
;; 参数:
;;   :curve-values_N  Float  ;; 各曲线的值 (动态数量)
;;   + 其他 pin 参数
;; Pose 输入:
;;   :source-pose  AnimNode  ;; 源姿态
(: modify-curve (->* ()
                     (#:source-pose AnimNode
                      ;; :curve-values_N 是动态的
                      )
                     AnimNode))

;; 约束节点
;; UE: UAnimGraphNode_Constraint
;; 导出示例:
;;   (constraint
;;     :component-pose (child ...))
;;
;; 参数 (自动提取):
;;   + 各类约束相关 pin 参数
;; Pose 输入:
;;   :component-pose  AnimNode  ;; 输入姿态
(: constraint (->* ()
                   (#:component-pose AnimNode)
                   AnimNode))

;; ============================================================
;;  空间转换
;; ============================================================

;; 组件空间转局部空间
;; UE: UAnimGraphNode_ComponentToLocalSpace
;; 导出示例:
;;   (component-to-local-space
;;     :component-pose (child ...))
;;
;; Pose 输入:
;;   :component-pose  AnimNode  ;; Component Space 姿态
(: component-to-local-space (->* ()
                                 (#:component-pose AnimNode)
                                 AnimNode))

;; 局部空间转组件空间
;; UE: UAnimGraphNode_LocalToComponentSpace
;; 导出示例:
;;   (local-to-component-space
;;     :local-pose (child ...))
;;
;; Pose 输入:
;;   :local-pose  AnimNode  ;; Local Space 姿态
(: local-to-component-space (->* ()
                                 (#:local-pose AnimNode)
                                 AnimNode))

;; ============================================================
;;  动画层/链接
;; ============================================================

;; 链接动画层
;; UE: UAnimGraphNode_LinkedAnimLayer
;; 导出示例:
;;   (linked-anim-layer
;;     :in-pose (child ...))
;;   (linked-anim-layer)  ;; 无输入
;;
;; Pose 输入 (可选):
;;   :in-pose  AnimNode  ;; 传入的姿态
(: linked-anim-layer (->* ()
                          (#:in-pose AnimNode)
                          AnimNode))

;; 链接输入姿态
;; UE: UAnimGraphNode_LinkedInputPose
;; 导出示例:
;;   (linked-input-pose)
;;
;; Pose 输入: 无 (占位符，由外部层提供)
(: linked-input-pose (-> AnimNode))

;; ============================================================
;;  Slot / 通知
;; ============================================================

;; 动画槽
;; UE: UAnimGraphNode_Slot
;; 导出示例:
;;   (slot :slot-name "DefaultSlot"
;;     :source (child ...))
;;
;; 参数:
;;   :slot-name  String  ;; Slot 名称
;;   + 其他 pin 参数
;; Pose 输入:
;;   :source  AnimNode  ;; 默认姿态（无 Montage 播放时使用）
(: slot (->* ()
             (#:slot-name String
              #:source AnimNode)
             AnimNode))

;; ============================================================
;;  其他常用节点 (通用回退格式)
;; ============================================================

;; Look At
;; UE: UAnimGraphNode_LookAt
;; 通用回退导出，参数自动从 pin 提取
(: look-at (->* ()
                (#:component-pose AnimNode)
                AnimNode))

;; Aim Offset
;; UE: UAnimGraphNode_AimOffsetLookAt / RotationOffsetBlendSpace
(: aim-offset (->* ()
                   (#:base-pose AnimNode)
                   AnimNode))

;; 参考姿态（T-Pose / 身份姿态）
;; UE: UAnimGraphNode_RefPose / IdentityPose
(: identity-pose (-> AnimNode))

;; ============================================================
;;  导出格式约定
;; ============================================================

;; 1. 所有节点名称使用 kebab-case (如 two-bone-i-k, blend-list)
;;
;; 2. 参数格式: :key value
;;    - Float:   :alpha 0.500000
;;    - Bool:    :loop true
;;    - String:  :name "Walk_Fwd"
;;    - Enum:    :blend-profile "MyProfile"
;;    - Ref:     :alpha (ref "Get SomeVariable")
;;
;; 3. Pose 输入格式:
;;    :pin-name
;;      (child-node ...)
;;
;; 4. 动态 pin (数组输入):
;;    :blend-pose_0 (child-0 ...)
;;    :blend-pose_1 (child-1 ...)
;;    :blend-time_0 0.400000
;;    :blend-time_1 0.500000
;;
;; 5. 通用回退:
;;    未列出的 UE 节点通过通用逻辑处理:
;;    - 类名 "AnimGraphNode_XYZ" → "x-y-z" (kebab-case)
;;    - 所有非 Struct input pin → 参数
;;    - 所有 Struct input pin → 命名子节点

;; ============================================================
;;  完整示例
;; ============================================================

;; 示例 1: 简单行走循环
;; (sequence-player :name "Walk_Fwd_Rifle" :loop true)

;; 示例 2: 混合空间
;; (blendspace-player :name "Locomotion_BS" :loop true
;;                    :x (ref "Get Speed") :y (ref "Get Direction"))

;; 示例 3: define 绑定 + 变量引用
;; (anim-blueprint "Example"
;;   :variables [...]
;;
;;   (define Post-Layering
;;     (layered-bone-blend :blend-weights-0 1.000000
;;       :base-pose (sequence-player :name "Idle" :loop true)
;;       :blend-poses-0 (linked-input-pose)))
;;
;;   :anim-graph
;;     (blend :alpha-curve-name "SpineRotation"
;;       :a (Post-Layering)            ;; ← 变量引用
;;       :b (modify-bone
;;             :component-pose
;;               (local-to-component-space
;;                 :local-pose (Post-Layering)))))  ;; ← 同一绑定引用两次

;; 示例 4: 手 IK 链 (使用 define)
;; (define Post-Layering (linked-anim-layer ...))
;;
;; (component-to-local-space
;;   :component-pose
;;     (two-bone-i-k :alpha (ref "Get Enable_HandIK_R")
;;       :component-pose
;;         (two-bone-i-k :alpha (ref "Get Enable_HandIK_L")
;;           :component-pose
;;             (local-to-component-space
;;               :local-pose (Post-Layering)))))    ;; ← 变量引用

;; 示例 5: 多个 define 形成依赖链
;; (define Main-Camera-States
;;   (state-machine :name "Main Camera States"))
;;
;; (define ShoulderSwap
;;   (blend-list ... :blend-pose-0 (Main-Camera-States) ...))
;;
;; (define MovementAction
;;   (blend-list ... :blend-pose-0 (ShoulderSwap) ...))
;;
;; :anim-graph
;;   (blend-list ... :blend-pose-0 (MovementAction) ...)

;; ============================================================
;;  导出
;; ============================================================

(provide sequence-player
         sequence-evaluator
         blendspace-player
         blend
         blend-list
         apply-additive
         apply-mesh-space-additive
         layered-bone-blend
         state-machine
         ;; save-cached-pose → (define Name body) — 不再作为节点
         ;; use-cached-pose  → (Name)             — 退化为变量引用
         two-bone-i-k
         modify-bone
         modify-curve
         constraint
         component-to-local-space
         local-to-component-space
         linked-anim-layer
         linked-input-pose
         slot
         look-at
         aim-offset
         identity-pose)

;; ============================================================
;;  节点清单 (23 个节点 + define 语法)
;; ============================================================

;; 基础播放: 3 个
;; - sequence-player
;; - sequence-evaluator
;; - blendspace-player

;; 混合: 5 个
;; - blend (TwoWayBlend)
;; - blend-list (ByBool/ByInt/ByEnum)
;; - apply-additive
;; - apply-mesh-space-additive
;; - layered-bone-blend

;; 状态机: 1 个
;; - state-machine

;; 定义绑定 (Lisp define 语义):
;; - (define Name body)    ← SaveCachedPose 提升为顶层绑定
;; - (Name)                ← UseCachedPose 退化为变量引用

;; 骨骼修改: 4 个
;; - two-bone-i-k
;; - modify-bone
;; - modify-curve
;; - constraint

;; 空间转换: 2 个
;; - component-to-local-space
;; - local-to-component-space

;; 层/链接: 2 个
;; - linked-anim-layer
;; - linked-input-pose

;; Slot: 1 个
;; - slot

;; 其他: 3 个
;; - look-at
;; - aim-offset
;; - identity-pose

;; 注意: 未列出的节点通过通用回退机制处理，
;; 自动提取所有 pin 参数和 pose 输入。
