#!/usr/bin/env racket
#lang racket

;; AnimLang Formatter - S-expression 漂亮打印
;; 用法: ./animlang-format.rkt <file.animlang>

(require racket/pretty)

;; 配置
(pretty-print-columns 80)
(pretty-print-depth #f)

(define (format-animlang file)
  (unless (file-exists? file)
    (displayln (format \"Error: File not found: ~a\" file))
    (exit 1))
  
  (with-handlers ([exn:fail:read?
                   (lambda (e)
                     (displayln (format \"Parse error: ~a\" (exn-message e)))
                     (exit 1))])
    (define sexp (call-with-input-file file read))
    (pretty-print sexp)))

;; 主程序
(define (main args)
  (when (null? args)
    (displayln \"Usage: animlang-format.rkt <file.animlang>\")
    (exit 1))
  
  (format-animlang (car args)))

(main (vector->list (current-command-line-arguments)))