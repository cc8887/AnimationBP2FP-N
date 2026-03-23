;; AnimLang Type Definitions (Stub File)
;; 类似于 TypeScript 的 .d.ts 或 Python 的 .pyi
;; 用于 IDE 自动补全、类型检查和文档生成

#lang typed/racket

;; ========== 基础类型 ==========

(define-type PinType
  (U 'Pose 'Float 'Int 'Bool 'Vector 'Rotator 'Transform 'Name 'Object))

(define-type AnimSequence String)  ;; 资产路径
(define-type BlendSpace String)
(define-type BoneName String)
(define-type SlotName String)

;; ========== 表达式类型 ==========

(define-type Expr
  (U Float
     Integer
     Boolean
     String
     Symbol  ;; :parameter-reference
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

;; 序列播放器
(: sequence-player (->* (AnimSequence)
                        (#:loop Boolean
                         #:play-rate (U Float Symbol)
                         #:start-position Float)
                        AnimNode))

;; 混合
(: blend (->* (Expr AnimNode AnimNode)
              (#:blend-time Float)
              AnimNode))

;; 混合空间 1D
(: blendspace-1d (->* (BlendSpace)
                      (#:axis Symbol
                       #:loop Boolean
                       #:play-rate (U Float Symbol))
                      AnimNode))

;; 混合空间 2D
(: blendspace-2d (->* (BlendSpace)
                      (#:x Symbol
                       #:y Symbol
                       #:loop Boolean
                       #:play-rate (U Float Symbol))
                      AnimNode))

;; 状态机
(: state-machine (->* (Symbol)
                      (#:initial Symbol
                       #:states (Listof State)
                       #:transitions (Listof Transition))
                      AnimNode))

;; 分层混合
(: layered-blend-per-bone (->* ()
                               (#:base AnimNode
                                #:layers (Listof BlendLayer))
                               AnimNode))

;; 槽
(: slot (->* (SlotName)
             (#:default AnimNode)
             AnimNode))

;; Aim Offset
(: aimoffset-2d (->* (String)
                     (#:x Symbol
                      #:y Symbol)
                     AnimNode))

;; IK
(: two-bone-ik (->* ()
                    (#:effector-location Expr
                     #:joint-target Expr)
                    AnimNode))

;; ========== 辅助结构 ==========

(struct State
  ([name : Symbol]
   [animation : AnimNode]
   [transition-to : (Option Symbol)]
   [on-finish : Boolean])
  #:transparent)

(struct Transition
  ([from-state : (U Symbol 'any)]
   [to-state : Symbol]
   [condition : Expr]
   [duration : Float]
   [interruptible : Boolean]
   [priority : Integer]
   [from-states : (Listof Symbol)])  ;; For 'any transitions
  #:transparent)

(struct BlendLayer
  ([name : String]
   [weight : (U Float Symbol)]
   [blend-mode : (U 'blend 'replace)]
   [bone-filter : BoneName]
   [blend-depth : Integer]
   [source : AnimNode])
  #:transparent)

;; ========== 变量定义 ==========

(struct VariableDef
  ([type : PinType]
   [name : Symbol]
   [default : Any]
   [range : (Option (List Float Float))]
   [description : (Option String)])
  #:transparent)

;; ========== 动画蓝图 ==========

(struct AnimBlueprint
  ([name : String]
   [variables : (Listof VariableDef)]
   [anim-graph : AnimNode]
   [layers : (Listof BlendLayer)]
   [anim-notifies : (Listof AnimNotify)])
  #:transparent)

(struct AnimNotify
  ([name : String]
   [callback : Symbol])
  #:transparent)

;; ========== 抽象类型 ==========

(define-type AnimNode Any)  ;; 暂定为 Any，实际使用时会推导具体类型

;; ========== 导出 ==========

(provide PinType
         AnimSequence
         BlendSpace
         BoneName
         SlotName
         Expr
         State
         Transition
         BlendLayer
         VariableDef
         AnimBlueprint
         AnimNotify
         AnimNode
         sequence-player
         blend
         blendspace-1d
         blendspace-2d
         state-machine
         layered-blend-per-bone
         slot
         aimoffset-2d
         two-bone-ik)