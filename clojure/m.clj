(defn- map-passing-context [func initial-context lis]
  (loop [context initial-context
         lis lis
         acc []]
    (if (empty? lis)
      [acc context]
    (let [this (first lis)
          remainder (next lis)
          [result new-context] (apply func [this context])]
      (recur new-context remainder (conj acc result))))))

(defmacro literal [x] `~x)

(defn- build-match*
  [k 
   { :keys [form bindings equal-checks not-nil is-nil] :as context 
    :or { bindings #{} equal-checks #{} not-nil #{} is-nil #{} } }]
  (let [g (gensym)]
  (cond
   (and (coll? k) (= 'quote (first k))) [g (assoc context :equal-checks (conj equal-checks `(= ~g ~k)))]
   (and (coll? k) (= 'literal (first k))) [g (assoc context :equal-checks (conj equal-checks `(= ~g ~(nth k 1))))]
   (coll? k) (let [has-body (some #(= % '&) k)
                   k* (if (not has-body) (conj k '& 'nil) k)
                   [nf ctx] (map-passing-context build-match* context k*)]
               ; if an explicit & body form was passed in, remove
               ; non-nil check on it if one didn't already exist
               [nf (if (and has-body (not ((context :not-nil #{}) (last nf))))
                     (assoc ctx :not-nil (disj (ctx :not-nil) (last nf)))
                     ctx)])
   (= k '_) [g (assoc context :not-nil (conj not-nil g))]
   (= k 'nil) [g (assoc context :is-nil (conj is-nil g))]
   (= k '&) ['& context]
   (or (string? k) (number? k) (keyword? k)) [g (assoc context :equal-checks (conj equal-checks `(= ~g ~k)))]
   (symbol? k) (if (bindings k)
                 [g (assoc context :equal-checks (conj equal-checks `(= ~g ~k)))]
                 [k (assoc context :not-nil (conj not-nil k) :bindings (conj bindings k))])
)))

;; given the variable and a set of match cases, build the macro.
(defn- build-match [v body]
  (if (< (count body) 2) nil
      (let [has-when (= (nth body 1) :when)
            condition (take (if has-when 4 2) body)
            rest-of-body (drop (if has-when 4 2) body)
            params (nth condition 0)
            [nf ctx] (build-match* params {})]
        (let [action (drop (if has-when 3 1) condition)
              when-check (if has-when (list (nth condition 2)))
              not-nil (map (fn [v] `(not (nil? ~v))) (ctx :not-nil))
              is-nil (map (fn [v] `(nil? ~v)) (ctx :is-nil))
              equal-checks (ctx :equal-checks)]
          `(let ~(vector nf v) 
             (if (and ~@not-nil ~@is-nil ~@equal-checks ~@when-check)
               ~@action
               ~(build-match v rest-of-body)))
          ))))

(defmacro match 
  "This performs simple pattern matching a la ML / Haskell.  The base
  syntax is (match value pattern action pattern2 action2 ...).
  The patterns follow the typical destructured bind syntax, but there
  are checks added to make the pattern matching stricter; for example,
  a pattern of [a b] will only match against a two element vector. The
  same symbol can be used multiple times to indicate equality, so [a
  a] means 'a vector of two elements, both the same.'
  The pattern :when condition action syntax can be used to specify a
  conditional that is checked before the pattern is invoked."  
  [v & body]
  (build-match v body))

(defmacro defnp
  "This defines a function taking a single value that goes through an
  implicit case statement.  For example: (defnp signum 0
  0 n :when (< n 0) -1 _ 1)"
  [fn-name & patterns]
  `(defn ~fn-name [x#] (match x# ~@patterns)))