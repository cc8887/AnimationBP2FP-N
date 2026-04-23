#!/usr/bin/env racket
#lang racket

;; AnimLang Linter - S-expression 验证工具
;; 用法: ./animlang-lint.rkt <file.animlang>

(require racket/match)

;; ========== 错误报告 ==========
(struct lint-error (message line form) #:transparent)

(define errors '())

(define (report-error msg form)
  (set! errors (cons (lint-error msg #f form) errors)))

;; ========== 主验证函数 ==========
(define (lint-animlang sexp)
  (match sexp
    [`(anim-blueprint ,name . ,rest)
     (unless (string? name)
       (report-error "Blueprint name must be a string" name))
     (check-blueprint-body rest)]
    [_
     (report-error "Root must be (anim-blueprint ...)" sexp)]))

;; ========== 蓝图主体验证 ==========
(define (check-blueprint-body forms)
  (define has-anim-graph #f)
  
  (for ([form forms])
    (match form
      [`(:variables . ,vars)
       (check-variables vars)]
      
      [`(:anim-graph . ,graph)
       (set! has-anim-graph #t)
       (check-anim-graph (car graph))]
      
      [`(:layers . ,layers)
       (for-each check-blend-layer layers)]
      
      [`(:anim-notifies . ,notifies)
       (for-each check-anim-notify notifies)]
      
      [_
       (report-error (format "Unknown top-level form: ~a" (car form)) form)]))
  
  (unless has-anim-graph
    (report-error "Missing :anim-graph section" forms)))

;; ========== 变量验证 ==========
(define (check-variables vars)
  (for ([var vars])
    (match var
      [`(,type ,keyword ,default . ,opts)
       (check-variable-type type)
       (check-keyword keyword)
       (check-variable-options opts)]
      [_
       (report-error "Invalid variable definition" var)])))

(define (check-variable-type type)
  (unless (member type '(float int bool vector rotator transform name))
    (report-error (format "Invalid variable type: ~a" type) type)))

(define (check-keyword kw)
  (unless (and (symbol? kw)
               (string-prefix? (symbol->string kw) ":"))
    (report-error (format "Variable name must be a keyword (e.g., :speed): ~a" kw) kw)))

(define (check-variable-options opts)
  (let loop ([opts opts])
    (match opts
      ['() (void)]
      [`(:range [,min ,max] . ,rest)
       (unless (and (number? min) (number? max))
         (report-error "Range values must be numbers" opts))
       (loop rest)]
      [`(:description ,desc . ,rest)
       (unless (string? desc)
         (report-error "Description must be a string" desc))
       (loop rest)]
      [_ (report-error "Invalid variable option" opts)])))

;; ========== 动画图验证 ==========
(define (check-anim-graph node)
  (match node
    ;; Sequence Player
    [`(sequence-player ,anim . ,opts)
     (unless (string? anim)
       (report-error "Animation name must be a string" anim))
     (check-node-options opts)]
    
    ;; Blend
    [`(blend ,alpha ,pose1 ,pose2 . ,opts)
     (check-expression alpha)
     (check-anim-graph pose1)
     (check-anim-graph pose2)]
    
    ;; BlendSpace 1D
    [`(blendspace-1d ,name :axis ,axis . ,opts)
     (unless (string? name)
       (report-error "BlendSpace name must be a string" name))
     (check-keyword axis)]
    
    ;; BlendSpace 2D
    [`(blendspace-2d ,name :x ,x :y ,y . ,opts)
     (unless (string? name)
       (report-error "BlendSpace name must be a string" name))
     (check-keyword x)
     (check-keyword y)]
    
    ;; State Machine
    [`(state-machine ,name . ,body)
     (check-keyword name)
     (check-state-machine body)]
    
    ;; Layered Blend
    [`(layered-blend-per-bone :base ,base :layers ,layers)
     (check-anim-graph base)
     (for-each check-blend-layer layers)]
    
    ;; Slot
    [`(slot ,name . ,opts)
     (unless (string? name)
       (report-error "Slot name must be a string" name))]
    
    [_
     (report-error (format "Unknown animation node type: ~a" (car node)) node)]))

;; ========== 状态机验证 ==========
(define (check-state-machine body)
  (define initial-state #f)
  (define states '())
  (define transitions '())
  
  (for ([form body])
    (match form
      [`(:initial ,state)
       (set! initial-state state)
       (check-keyword state)]
      
      [`(:states . ,state-list)
       (set! states state-list)
       (for-each check-state state-list)]
      
      [`(:transitions . ,trans-list)
       (set! transitions trans-list)
       (for-each check-transition trans-list)]
      
      [_
       (report-error "Invalid state machine form" form)]))
  
  (unless initial-state
    (report-error "State machine missing :initial" body))
  
  (when (null? states)
    (report-error "State machine has no states" body)))

(define (check-state state)
  (match state
    [`(state ,name . ,body)
     (check-keyword name)
     (when (not (null? body))
       (check-anim-graph (car body)))]
    [_
     (report-error "Invalid state definition" state)]))

(define (check-transition trans)
  (match trans
    [`(transition ,from ,to :condition ,cond . ,opts)
     (check-keyword from)
     (check-keyword to)
     (check-expression cond)]
    [_
     (report-error "Invalid transition definition" trans)]))

;; ========== 表达式验证 ==========
(define (check-expression expr)
  (match expr
    ;; 字面量
    [(? number?) (void)]
    [(? boolean?) (void)]
    [(? string?) (void)]
    
    ;; 关键字（参数引用）
    [(? symbol?)
     (when (not (string-prefix? (symbol->string expr) ":"))
       (report-error "Parameter reference must start with :" expr))]
    
    ;; 二元运算
    [`(,op ,left ,right)
     #:when (member op '(+ - * / > < >= <= = and or))
     (check-expression left)
     (check-expression right)]
    
    ;; 一元运算
    [`(not ,expr)
     (check-expression expr)]
    
    ;; 条件
    [`(if ,cond ,then ,else)
     (check-expression cond)
     (check-expression then)
     (check-expression else)]
    
    [_
     (report-error (format "Invalid expression: ~a" expr) expr)]))

;; ========== 其他节点验证 ==========
(define (check-blend-layer layer)
  (match layer
    [`(blend-layer . ,opts)
     (void)]
    [_
     (report-error "Invalid blend layer" layer)]))

(define (check-anim-notify notify)
  (match notify
    [`(notify ,name :callback ,callback)
     (unless (string? name)
       (report-error "Notify name must be a string" name))]
    [_
     (report-error "Invalid anim notify" notify)]))

(define (check-node-options opts)
  (void))  ;; TODO: 实现选项验证

;; ========== 主程序 ==========
(define (main args)
  (when (null? args)
    (displayln "Usage: animlang-lint.rkt <file.animlang>")
    (exit 1))
  
  (define file (car args))
  
  (unless (file-exists? file)
    (displayln (format "Error: File not found: ~a" file))
    (exit 1))
  
  (set! errors '())
  
  (with-handlers ([exn:fail:read?
                   (lambda (e)
                     (displayln (format "Parse error: ~a" (exn-message e)))
                     (exit 1))])
    (define sexp (call-with-input-file file read))
    (lint-animlang sexp))
  
  (if (null? errors)
      (begin
        (displayln "✓ No errors found")
        (exit 0))
      (begin
        (displayln (format "✗ Found ~a error(s):" (length errors)))
        (for ([err (reverse errors)])
          (displayln (format "  - ~a" (lint-error-message err))))
        (exit 1))))

;; 运行
(main (vector->list (current-command-line-arguments)))