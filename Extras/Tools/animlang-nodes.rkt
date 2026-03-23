;; AnimLang Node Library (手动整理版)
;; 基于 UE5.6 动画系统常用节点
;; 这是从 UE 引擎导出的类型定义参考

#lang typed/racket

(require \"animlang-types.rkt\")

;; ========== 基础动画播放 ==========

;; 序列播放器 - 播放单个动画序列
;; UE: UAnimGraphNode_SequencePlayer
(: sequence-player (->* (AnimSequence)
                        (#:loop Boolean
                         #:play-rate (U Float Symbol)
                         #:start-position Float
                         #:blend-in-time Float
                         #:blend-out-time Float)
                        AnimNode))

;; 混合空间播放器 (1D)
;; UE: UAnimGraphNode_BlendSpacePlayer
(: blendspace-1d (->* (BlendSpace)
                      (#:axis Symbol              ; 输入参数（如 :speed）
                       #:loop Boolean
                       #:play-rate (U Float Symbol))
                      AnimNode))

;; 混合空间播放器 (2D)
;; UE: UAnimGraphNode_BlendSpacePlayer
(: blendspace-2d (->* (BlendSpace)
                      (#:x Symbol                 ; X 轴输入（如 :speed）
                       #:y Symbol                 ; Y 轴输入（如 :direction）
                       #:loop Boolean
                       #:play-rate (U Float Symbol))
                      AnimNode))

;; Aim Offset (瞄准偏移)
;; UE: UAnimGraphNode_AimOffsetLookAt
(: aimoffset-2d (->* (String)                     ; AimOffset 资产
                     (#:x Symbol                  ; Yaw
                      #:y Symbol                  ; Pitch
                      #:base-pose (Option AnimNode))
                     AnimNode))

;; ========== 混合节点 ==========

;; 二路混合 - 按 Alpha 混合两个姿态
;; UE: UAnimGraphNode_BlendListByBool
(: blend (->* ((U Float Symbol) AnimNode AnimNode)
              (#:blend-time Float)
              AnimNode))

;; 多路混合 - 按索引选择姿态
;; UE: UAnimGraphNode_BlendListByInt
(: blend-list (->* ((U Integer Symbol) (Listof AnimNode))
                   (#:blend-time Float)
                   AnimNode))

;; 按布尔值混合
;; UE: UAnimGraphNode_BlendListByBool
(: blend-by-bool (->* ((U Boolean Symbol) AnimNode AnimNode)
                      (#:blend-time Float)
                      AnimNode))

;; 分层混合（按骨骼）
;; UE: UAnimGraphNode_LayeredBoneBlend
(: layered-blend-per-bone (->* ()
                               (#:base AnimNode
                                #:layers (Listof BlendLayer))
                               AnimNode))

;; 叠加混合
;; UE: UAnimGraphNode_ApplyAdditive
(: apply-additive (->* (AnimNode AnimNode)      ; Base + Additive
                       (#:alpha (U Float Symbol)
                        #:lod-threshold Integer)
                       AnimNode))

;; ========== 状态机 ==========

;; 状态机
;; UE: UAnimGraphNode_StateMachine
(: state-machine (->* (Symbol)                   ; 状态机名称
                      (#:initial Symbol          ; 初始状态
                       #:states (Listof State)
                       #:transitions (Listof Transition))
                      AnimNode))

;; ========== 修改器 ==========

;; 槽（动画通知槽）
;; UE: UAnimGraphNode_Slot
(: slot (->* (SlotName)
             (#:default (Option AnimNode))
             AnimNode))

;; 两骨骼 IK
;; UE: UAnimGraphNode_TwoBoneIK
(: two-bone-ik (->* ()
                    (#:effector-location Expr    ; 目标位置
                     #:joint-target Expr         ; 关节目标
                     #:effector-bone BoneName
                     #:joint-bone BoneName)
                    AnimNode))

;; 骨骼修改（变换）
;; UE: UAnimGraphNode_ModifyBone
(: modify-bone (->* (BoneName)
                    (#:translation Expr
                     #:rotation Expr
                     #:scale Expr
                     #:translation-space Symbol  ; 'component, 'bone, 'world
                     #:rotation-space Symbol)
                    AnimNode))

;; Look At（注视目标）
;; UE: UAnimGraphNode_LookAt
(: look-at (->* (BoneName)                       ; 要旋转的骨骼
                (#:target Expr                   ; 目标位置
                 #:look-at-axis Symbol           ; 'x, 'y, 'z
                 #:interpolation-speed Float)
                AnimNode))

;; ========== 缓存节点 ==========

;; 保存缓存姿态
;; UE: UAnimGraphNode_SaveCachedPose
(: save-cached-pose (->* (AnimNode Symbol)       ; 姿态 + 缓存名
                         ()
                         AnimNode))

;; 使用缓存姿态
;; UE: UAnimGraphNode_UseCachedPose
(: use-cached-pose (Symbol -> AnimNode))         ; 缓存名 -> 姿态

;; ========== 混合时间曲线 ==========

;; 时间混合
;; UE: UAnimGraphNode_BlendPosesByTime
(: blend-by-time (->* ((Listof AnimNode))
                      (#:time (U Float Symbol))
                      AnimNode))

;; ========== 实用工具 ==========

;; 参考姿态（T-Pose）
;; UE: UAnimGraphNode_RefPose
(: reference-pose (-> AnimNode))

;; 组件到局部空间
;; UE: UAnimGraphNode_ComponentToLocalSpace
(: component-to-local (AnimNode -> AnimNode))

;; 局部到组件空间
;; UE: UAnimGraphNode_LocalToComponentSpace
(: local-to-component (AnimNode -> AnimNode))

;; ========== 完整示例 ==========

;; 示例 1: 简单行走循环
(define walk-anim
  (sequence-player \"Walk_Fwd_Rifle\"
                   #:loop #t
                   #:play-rate 1.0))

;; 示例 2: 速度混合空间
(define locomotion
  (blendspace-2d \"Locomotion_BS\"
                 #:x :speed
                 #:y :direction
                 #:loop #t))

;; 示例 3: 上下身分层
(define full-body
  (layered-blend-per-bone
    #:base (state-machine :locomotion
                          #:initial :idle
                          #:states [...])
    #:layers [(BlendLayer \"UpperBody\"
                          1.0
                          'blend
                          \"spine_01\"
                          -1
                          (slot \"UpperBodyShoot\"))]))

;; ========== 导出 ==========

(provide sequence-player
         blendspace-1d
         blendspace-2d
         aimoffset-2d
         blend
         blend-list
         blend-by-bool
         layered-blend-per-bone
         apply-additive
         state-machine
         slot
         two-bone-ik
         modify-bone
         look-at
         save-cached-pose
         use-cached-pose
         blend-by-time
         reference-pose
         component-to-local
         local-to-component)

;; ========== 节点列表 ==========

;; 基础播放: 3 个节点
;; - sequence-player
;; - blendspace-1d
;; - blendspace-2d

;; 混合: 5 个节点
;; - blend
;; - blend-list
;; - blend-by-bool
;; - layered-blend-per-bone
;; - apply-additive

;; 状态机: 1 个节点
;; - state-machine

;; 修改器: 4 个节点
;; - slot
;; - two-bone-ik
;; - modify-bone
;; - look-at

;; 缓存: 2 个节点
;; - save-cached-pose
;; - use-cached-pose

;; 实用工具: 3 个节点
;; - reference-pose
;; - component-to-local
;; - local-to-component

;; 总计: 18 个核心节点