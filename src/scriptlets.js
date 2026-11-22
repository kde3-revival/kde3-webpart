/*
    This file is part of kde3-wpepart, a WPE WebKit browser part for KDE 3.

    Copyright (C) 2026 Serghei Amelian

    This library is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation; either version 2.1 of the License, or
    (at your option) any later version.
*/

/*
 * Scriptlets: the small page patches uBlock Origin's filter lists ask for
 * with "site##+js(name, args...)".  Written for kde3-wpepart from the
 * documented behaviour of those filters (no uBlock code: it is GPLv3).
 *
 * The part puts this file into a WebKit user script, one per site, run
 * in the page's own world before any script of the page, in every frame
 * of that site:
 *
 *     (function () { <this file> run([[name, args...], ...]); })();
 *
 * Everything stays inside that function: nothing of ours is visible to
 * the page, except the patched functions and properties themselves.
 */

'use strict';

// What the page may replace later: taken now.
const W = window;
const ObjectDefineProperty = Object.defineProperty;
const ObjectGetOwnPropertyDescriptor = Object.getOwnPropertyDescriptor;
const ObjectKeys = Object.keys;
const ReflectApply = Reflect.apply;
const ArrayIsArray = Array.isArray;
const JSONParse = JSON.parse;
const JSONStringify = JSON.stringify;
const FunctionToString = Function.prototype.toString;
const NativeProxy = Proxy;
const NativeRegExp = RegExp;
const NativeError = Error;
const NativeReferenceError = ReferenceError;
const NativePromise = Promise;
const NativeResponse = W.Response;
const NativeMutationObserver = W.MutationObserver;
const NativeHTMLScriptElement = W.HTMLScriptElement;
const NativeSetTimeout = W.setTimeout;
const regExpTest = RegExp.prototype.test;
const stringReplace = String.prototype.replace;
const magic = String.fromCharCode( 97 + Math.floor( Math.random() * 26 ) ) + Math.random().toString( 36 ).slice( 2 );

function test( re, text )
{
    re.lastIndex = 0;
    return ReflectApply( regExpTest, re, [ text ] );
}

function replace( text, re, replacement )
{
    re.lastIndex = 0;
    return ReflectApply( stringReplace, text, [ re, replacement ] );
}

function isObject( v )
{
    return v !== null && ( typeof v === 'object' || typeof v === 'function' );
}

function escapeRegExp( s )
{
    return s.replace( /[.*+?^${}()|[\]\\]/g, '\\$&' );
}

// "/re/flags": that regular expression (its own flags); anything else:
// the literal text (with @p flags).  "": everything.  null: invalid.
function toRegExp( pattern, flags )
{
    if ( pattern === '' )
        return /^/;
    const m = /^\/(.+)\/([dgimsuvy]*)$/.exec( pattern );
    try {
        if ( m )
            return new NativeRegExp( m[1], m[2] || undefined );
        return new NativeRegExp( escapeRegExp( pattern ), flags || undefined );
    } catch ( e ) {
        return null;
    }
}

// A pattern that may start with "!" (does not match): a test function,
// null if invalid.
function matcher( pattern )
{
    let negate = false;
    if ( pattern.startsWith( '!' ) ) {
        negate = true;
        pattern = pattern.slice( 1 );
    }
    const re = toRegExp( pattern );
    if ( !re )
        return null;
    return text => test( re, text ) !== negate;
}

// "key, value, key, value" after the fixed arguments
function extraArgs( args, from )
{
    const extra = {};
    for ( let i = from; i + 1 < args.length; i += 2 )
        extra[args[i]] = args[i + 1];
    return extra;
}

function stackText()
{
    let stack = '';
    try {
        stack = new NativeError().stack || '';
    } catch ( e ) {
    }
    // uBlock's tokens: frames of the document's inline scripts
    const doc = W.location.href.split( '#' )[0];
    return stack.split( '\n' ).map( line => line.includes( doc + ':' ) ? line + ' inlineScript' : line ).join( '\n' );
}

function stackMatcher( needle )
{
    if ( !needle )
        return null;
    const m = matcher( needle );
    return m ? () => m( stackText() ) : () => false;
}

// "url:/re/ method:POST" or just a URL pattern: the properties of a
// request to match; null if invalid.
const requestProps = [ 'url', 'method', 'headers', 'body', 'mode', 'credentials', 'cache', 'redirect', 'referrer',
                       'referrerPolicy', 'integrity', 'keepalive', 'signal', 'priority' ];
function propsMatcher( text )
{
    const tests = [];
    for ( const token of text.split( /\s+/ ) ) {
        if ( token === '' )
            continue;
        const colon = token.indexOf( ':' );
        let key = 'url', value = token;
        if ( colon > 0 && requestProps.includes( token.slice( 0, colon ) ) ) {
            key = token.slice( 0, colon );
            value = token.slice( colon + 1 );
        }
        const re = toRegExp( value );
        if ( !re )
            return null;
        tests.push( [ key, re ] );
    }
    return details => {
        for ( const [ key, re ] of tests ) {
            const v = details[key];
            if ( v === undefined || !test( re, String( v ) ) )
                return false;
        }
        return true;
    };
}

// ---------------------------------------------------------------------
// properties: chains like "a.b.c", trapped before they exist

function trapChain( owner, chain, final )
{
    const dot = chain.indexOf( '.' );
    if ( dot < 0 ) {
        final( owner, chain );
        return;
    }
    const prop = chain.slice( 0, dot );
    const rest = chain.slice( dot + 1 );
    let value;
    try {
        value = owner[prop];
    } catch ( e ) {
        return;
    }
    if ( isObject( value ) ) {
        trapChain( value, rest, final );
        return;
    }
    // not there yet: when it is set
    const desc = ObjectGetOwnPropertyDescriptor( owner, prop );
    if ( desc && !desc.configurable )
        return;
    const prevGet = desc && desc.get, prevSet = desc && desc.set;
    ObjectDefineProperty( owner, prop, {
        configurable: true,
        enumerable: desc ? desc.enumerable : true,
        get() {
            return prevGet ? ReflectApply( prevGet, this, [] ) : value;
        },
        set( v ) {
            if ( prevSet )
                ReflectApply( prevSet, this, [ v ] );
            value = v;
            if ( isObject( v ) )
                trapChain( v, rest, final );
        }
    } );
}

function constantValue( raw, trusted )
{
    const named = {
        'undefined': [ undefined ], 'false': [ false ], 'true': [ true ], 'null': [ null ],
        '': [ '' ], "''": [ '' ], 'emptyStr': [ '' ],
        'noopFunc': [ function () {} ], 'trueFunc': [ function () { return true; } ],
        'falseFunc': [ function () { return false; } ],
        'throwFunc': [ function () { throw new NativeError(); } ],
        'noopCallbackFunc': [ function () { return function () {}; } ],
        'noopPromiseResolve': [ function () { return NativePromise.resolve(); } ],
        'noopPromiseReject': [ function () { return NativePromise.reject(); } ],
        'emptyObj': [ {} ], 'emptyArr': [ [] ], 'NaN': [ NaN ], 'Infinity': [ Infinity ], '-Infinity': [ -Infinity ],
        'yes': [ 'yes' ], 'no': [ 'no' ], 'on': [ 'on' ], 'off': [ 'off' ], 'ok': [ 'ok' ],
        'accept': [ 'accept' ], 'reject': [ 'reject' ], 'allow': [ 'allow' ], 'deny': [ 'deny' ]
    };
    if ( Object.prototype.hasOwnProperty.call( named, raw ) )
        return named[raw];
    if ( /^-?\d+$/.test( raw ) ) {
        const n = parseInt( raw, 10 );
        if ( n >= -32767 && n <= 32767 )
            return [ n ];
    }
    if ( trusted ) {
        try {
            return [ JSONParse( raw ) ];
        } catch ( e ) {
            return [ raw ];
        }
    }
    return null;
}

function setConstant( args, trusted )
{
    const [ chain = '', raw = '' ] = args;
    if ( chain === '' )
        return;
    const c = constantValue( raw, trusted );
    if ( !c )
        return;
    const value = c[0];
    // A page that puts something of another type there does not get
    // ours (it would break).
    const foreign = v => v !== undefined && v !== null && value !== undefined && value !== null && typeof v !== typeof value;
    trapChain( W, chain, ( owner, prop ) => {
        const desc = ObjectGetOwnPropertyDescriptor( owner, prop );
        if ( desc && !desc.configurable )
            return;
        if ( desc && 'value' in desc && foreign( desc.value ) )
            return;
        let own = false, ownValue;
        ObjectDefineProperty( owner, prop, {
            configurable: true,
            enumerable: desc ? desc.enumerable : true,
            get() {
                return own ? ownValue : value;
            },
            set( v ) {
                if ( foreign( v ) ) {
                    own = true;
                    ownValue = v;
                }
            }
        } );
    } );
}

function abortOnPropertyRead( args )
{
    const [ chain = '' ] = args;
    if ( chain === '' )
        return;
    trapChain( W, chain, ( owner, prop ) => {
        const desc = ObjectGetOwnPropertyDescriptor( owner, prop );
        if ( desc && !desc.configurable )
            return;
        ObjectDefineProperty( owner, prop, {
            configurable: true,
            get() {
                throw new NativeReferenceError( magic );
            },
            set() {}
        } );
    } );
}

function abortOnPropertyWrite( args )
{
    const [ chain = '' ] = args;
    if ( chain === '' )
        return;
    trapChain( W, chain, ( owner, prop ) => {
        const desc = ObjectGetOwnPropertyDescriptor( owner, prop );
        if ( desc && !desc.configurable )
            return;
        let value = desc && 'value' in desc ? desc.value : undefined;
        ObjectDefineProperty( owner, prop, {
            configurable: true,
            get() {
                return value;
            },
            set() {
                throw new NativeReferenceError( magic );
            }
        } );
    } );
}

// Accesses to the property from some scripts throw.
function abortWhen( chain, shouldAbort )
{
    trapChain( W, chain, ( owner, prop ) => {
        const desc = ObjectGetOwnPropertyDescriptor( owner, prop );
        if ( desc && !desc.configurable )
            return;
        let value = desc && 'value' in desc ? desc.value : undefined;
        const prevGet = desc && desc.get, prevSet = desc && desc.set;
        ObjectDefineProperty( owner, prop, {
            configurable: true,
            enumerable: desc ? desc.enumerable : true,
            get() {
                if ( shouldAbort() )
                    throw new NativeReferenceError( magic );
                return prevGet ? ReflectApply( prevGet, this, [] ) : value;
            },
            set( v ) {
                if ( shouldAbort() )
                    throw new NativeReferenceError( magic );
                if ( prevSet )
                    ReflectApply( prevSet, this, [ v ] );
                else
                    value = v;
            }
        } );
    } );
}

function abortCurrentScript( args )
{
    const [ chain = '', needle = '', context = '' ] = args;
    if ( chain === '' )
        return;
    const reNeedle = toRegExp( needle ), reContext = toRegExp( context );
    if ( !reNeedle || !reContext )
        return;
    abortWhen( chain, () => {
        const e = document.currentScript;
        if ( !( e instanceof NativeHTMLScriptElement ) )
            return false;
        if ( context !== '' && !test( reContext, e.src ) )
            return false;
        // the text of inline scripts; one loaded from a URL has none here
        return test( reNeedle, e.src ? '' : e.text );
    } );
}

function abortOnStackTrace( args )
{
    const [ chain = '', needle = '' ] = args;
    if ( chain === '' || needle === '' )
        return;
    const m = stackMatcher( needle );
    if ( m )
        abortWhen( chain, m );
}

// ---------------------------------------------------------------------
// timers, eval, window.open, addEventListener

const timerHooks = { setTimeout: [], setInterval: [] };

function hookTimer( name )
{
    const hooks = timerHooks[name];
    if ( hooks.installed )
        return hooks;
    hooks.installed = true;
    W[name] = new NativeProxy( W[name], {
        apply( target, thisArg, args ) {
            const fn = args[0];
            let text;
            try {
                text = typeof fn === 'function' ? ReflectApply( FunctionToString, fn, [] ) : String( fn );
            } catch ( e ) {
                text = '';
            }
            for ( const hook of hooks ) {
                if ( hook( args, text ) === false ) {
                    args[0] = function () {};
                    break;
                }
            }
            return ReflectApply( target, thisArg, args );
        }
    } );
    return hooks;
}

function delayMatcher( delay )
{
    if ( delay === '' )
        return () => true;
    let negate = false;
    if ( delay.startsWith( '!' ) ) {
        negate = true;
        delay = delay.slice( 1 );
    }
    const n = parseInt( delay, 10 );
    return d => ( ( Number( d ) || 0 ) === n ) !== negate;
}

function noTimerIf( name, args )
{
    const [ needle = '', delay = '' ] = args;
    if ( needle === '' && delay === '' )
        return;
    const m = matcher( needle );
    if ( !m )
        return;
    const d = delayMatcher( delay );
    hookTimer( name ).push( ( a, text ) => !( m( text ) && d( a[1] ) ) );
}

function adjustTimer( name, args )
{
    const [ needle = '', delay = '1000', boost = '0.05' ] = args;
    const m = matcher( needle );
    if ( !m )
        return;
    const any = delay === '*';
    const n = parseInt( delay, 10 );
    let b = parseFloat( boost );
    if ( isNaN( b ) || !isFinite( b ) )
        b = 0.05;
    b = Math.min( 50, Math.max( 0.001, b ) );
    hookTimer( name ).push( ( a, text ) => {
        if ( ( any || ( Number( a[1] ) || 0 ) === n ) && m( text ) )
            a[1] = ( Number( a[1] ) || 0 ) * b;
        return true;
    } );
}

function noevalIf( args )
{
    const [ needle = '' ] = args;
    const m = matcher( needle );
    if ( !m )
        return;
    W.eval = new NativeProxy( W.eval, {
        apply( target, thisArg, a ) {
            if ( m( String( a[0] ) ) )
                return undefined;
            return ReflectApply( target, thisArg, a );
        }
    } );
}

function preventWindowOpen( args )
{
    const [ needle = '' ] = args;
    const m = matcher( needle );
    if ( !m )
        return;
    W.open = new NativeProxy( W.open, {
        apply( target, thisArg, a ) {
            if ( !m( String( a[0] === undefined ? '' : a[0] ) ) )
                return ReflectApply( target, thisArg, a );
            // something that looks like a window that was opened
            const noop = function () {};
            return { closed: false, opener: W, location: { href: String( a[0] || '' ) },
                     close: noop, focus: noop, blur: noop, postMessage: noop, addEventListener: noop,
                     removeEventListener: noop, document: { write: noop, writeln: noop, close: noop } };
        }
    } );
}

function preventAddEventListener( args )
{
    const [ type = '', needle = '' ] = args;
    if ( type === '' && needle === '' )
        return;
    const reType = toRegExp( type );
    const m = matcher( needle );
    if ( !reType || !m )
        return;
    const proto = W.EventTarget.prototype;
    proto.addEventListener = new NativeProxy( proto.addEventListener, {
        apply( target, thisArg, a ) {
            let text = '';
            try {
                const h = a[1];
                text = typeof h === 'function' ? ReflectApply( FunctionToString, h, [] )
                    : isObject( h ) && typeof h.handleEvent === 'function' ? ReflectApply( FunctionToString, h.handleEvent, [] )
                    : String( h );
            } catch ( e ) {
            }
            if ( test( reType, String( a[0] ) ) && m( text ) )
                return undefined;
            return ReflectApply( target, thisArg, a );
        }
    } );
}

// ---------------------------------------------------------------------
// JSON: pruning parsed objects

// "a.b.c", "[]" or "*" (every element), "[-]" (array elements in which
// the rest of the path exists are removed)
function compilePaths( text )
{
    return text.split( / +/ ).filter( p => p !== '' ).map( p => p.split( '.' ) );
}

function hasPath( obj, path, i )
{
    if ( i === path.length )
        return true;
    if ( !isObject( obj ) )
        return false;
    const seg = path[i];
    if ( seg === '[]' || seg === '*' || seg === '[-]' ) {
        for ( const k of ObjectKeys( obj ) )
            if ( hasPath( obj[k], path, i + 1 ) )
                return true;
        return false;
    }
    if ( !( seg in obj ) )
        return false;
    return hasPath( obj[seg], path, i + 1 );
}

function prunePath( obj, path, i )
{
    if ( !isObject( obj ) )
        return false;
    const seg = path[i];
    const last = i === path.length - 1;
    if ( seg === '[-]' ) {
        if ( !ArrayIsArray( obj ) || last )
            return false;
        let removed = false;
        for ( let k = obj.length - 1; k >= 0; --k ) {
            if ( hasPath( obj[k], path, i + 1 ) ) {
                obj.splice( k, 1 );
                removed = true;
            }
        }
        return removed;
    }
    if ( seg === '[]' || seg === '*' ) {
        let any = false;
        for ( const k of ObjectKeys( obj ) ) {
            if ( last ) {
                delete obj[k];
                any = true;
            } else if ( prunePath( obj[k], path, i + 1 ) ) {
                any = true;
            }
        }
        return any;
    }
    if ( !Object.prototype.hasOwnProperty.call( obj, seg ) )
        return false;
    if ( last ) {
        delete obj[seg];
        return true;
    }
    return prunePath( obj[seg], path, i + 1 );
}

// a function that prunes an object in place (true: something went)
function makePruner( pruneText, needleText )
{
    const prune = compilePaths( pruneText );
    const needles = compilePaths( needleText );
    if ( prune.length === 0 )
        return null;
    return obj => {
        if ( !isObject( obj ) )
            return false;
        for ( const n of needles )
            if ( !hasPath( obj, n, 0 ) )
                return false;
        let any = false;
        for ( const p of prune )
            if ( prunePath( obj, p, 0 ) )
                any = true;
        return any;
    };
}

const jsonPruners = [];

function pruneParsed( obj )
{
    for ( const p of jsonPruners ) {
        try {
            if ( !p.stack || p.stack() )
                p.prune( obj );
        } catch ( e ) {
        }
    }
    return obj;
}

function jsonPrune( args )
{
    const [ prunePaths = '', needlePaths = '', stackNeedle = '' ] = args;
    const prune = makePruner( prunePaths, needlePaths );
    if ( !prune )
        return;
    jsonPruners.push( { prune, stack: stackMatcher( stackNeedle ) } );
    if ( jsonPruners.length > 1 )
        return;
    JSON.parse = new NativeProxy( JSON.parse, {
        apply( target, thisArg, a ) {
            return pruneParsed( ReflectApply( target, thisArg, a ) );
        }
    } );
    if ( NativeResponse ) {
        const proto = NativeResponse.prototype;
        proto.json = new NativeProxy( proto.json, {
            apply( target, thisArg, a ) {
                return ReflectApply( target, thisArg, a ).then( pruneParsed );
            }
        } );
    }
}

// ---------------------------------------------------------------------
// fetch() and XMLHttpRequest: requests prevented, responses edited

// A response edit: { match(details), edit(text) -> text or null (no change) }
const fetchEdits = [];
const fetchPrevents = [];
const xhrEdits = [];
const xhrPrevents = [];

function fetchDetails( args )
{
    const details = {};
    const input = args[0];
    if ( W.Request && input instanceof W.Request ) {
        for ( const k of requestProps )
            try {
                details[k] = input[k];
            } catch ( e ) {
            }
    } else {
        details.url = String( input );
        details.method = 'GET';
    }
    const init = args[1];
    if ( isObject( init ) )
        for ( const k of requestProps )
            if ( k !== 'url' && init[k] !== undefined )
                details[k] = init[k];
    if ( typeof details.method === 'string' )
        details.method = details.method.toUpperCase();
    return details;
}

// A response with the same status, headers, url and type, another body.
function rebuiltResponse( original, text )
{
    const response = new NativeResponse( text, { status: original.status, statusText: original.statusText,
                                                 headers: original.headers } );
    for ( const k of [ 'url', 'redirected', 'type' ] )
        try {
            ObjectDefineProperty( response, k, { value: original[k] } );
        } catch ( e ) {
        }
    return response;
}

function hookFetch()
{
    if ( hookFetch.done || !W.fetch || !NativeResponse )
        return;
    hookFetch.done = true;
    W.fetch = new NativeProxy( W.fetch, {
        apply( target, thisArg, args ) {
            if ( fetchPrevents.length === 0 && fetchEdits.length === 0 )
                return ReflectApply( target, thisArg, args );
            let details;
            try {
                details = fetchDetails( args );
            } catch ( e ) {
                return ReflectApply( target, thisArg, args );
            }
            for ( const p of fetchPrevents ) {
                if ( p.match( details ) ) {
                    const response = new NativeResponse( p.body, { status: 200, statusText: 'OK' } );
                    try {
                        ObjectDefineProperty( response, 'url', { value: details.url } );
                    } catch ( e ) {
                    }
                    return NativePromise.resolve( response );
                }
            }
            const edits = fetchEdits.filter( e => e.match( details ) );
            const result = ReflectApply( target, thisArg, args );
            if ( edits.length === 0 )
                return result;
            return result.then( response => {
                // no body to read or none that may be read
                if ( response.type === 'opaque' || [ 101, 103, 204, 205, 304 ].includes( response.status ) )
                    return response;
                return response.clone().text().then( text => {
                    let changed = false;
                    for ( const e of edits ) {
                        const out = e.edit( text );
                        if ( out !== null ) {
                            text = out;
                            changed = true;
                        }
                    }
                    return changed ? rebuiltResponse( response, text ) : response;
                } ).catch( () => response );
            } );
        }
    } );
}

const xhrRequests = new WeakMap();

function hookXHR()
{
    if ( hookXHR.done || !W.XMLHttpRequest )
        return;
    hookXHR.done = true;
    const proto = W.XMLHttpRequest.prototype;
    proto.open = new NativeProxy( proto.open, {
        apply( target, thisArg, args ) {
            xhrRequests.set( thisArg, { method: String( args[0] ).toUpperCase(), url: String( args[1] ), edited: null } );
            return ReflectApply( target, thisArg, args );
        }
    } );
    proto.send = new NativeProxy( proto.send, {
        apply( target, thisArg, args ) {
            const r = xhrRequests.get( thisArg );
            if ( r && xhrPrevents.some( p => p.match( r ) ) ) {
                prevented( thisArg, r );
                return undefined;
            }
            return ReflectApply( target, thisArg, args );
        }
    } );
    // The response, edited once it is complete.
    const editedText = xhr => {
        const r = xhrRequests.get( xhr );
        if ( !r || xhr.readyState !== 4 )
            return null;
        if ( r.edited === null ) {
            r.edited = false;
            const edits = xhrEdits.filter( e => e.match( r ) );
            const type = xhr.responseType;
            if ( edits.length && ( type === '' || type === 'text' || type === 'json' ) ) {
                let text;
                try {
                    text = type === 'json' ? JSONStringify( ReflectApply( responseGet, xhr, [] ) )
                        : ReflectApply( textGet, xhr, [] );
                } catch ( e ) {
                    text = undefined;
                }
                let changed = false;
                for ( const e of typeof text === 'string' ? edits : [] ) {
                    const out = e.edit( text );
                    if ( out !== null ) {
                        text = out;
                        changed = true;
                    }
                }
                if ( changed ) {
                    if ( type === 'json' ) {
                        try {
                            r.edited = { value: JSONParse( text ) };
                        } catch ( e ) {
                            r.edited = { value: null };
                        }
                    } else {
                        r.edited = { value: text };
                    }
                }
            }
        }
        return r.edited || null;
    };
    const responseGet = ObjectGetOwnPropertyDescriptor( proto, 'response' ).get;
    const textGet = ObjectGetOwnPropertyDescriptor( proto, 'responseText' ).get;
    ObjectDefineProperty( proto, 'response', {
        configurable: true, enumerable: true,
        get: new NativeProxy( responseGet, {
            apply( target, thisArg, args ) {
                const e = xhrEdits.length ? editedText( thisArg ) : null;
                return e ? e.value : ReflectApply( target, thisArg, args );
            }
        } )
    } );
    ObjectDefineProperty( proto, 'responseText', {
        configurable: true, enumerable: true,
        get: new NativeProxy( textGet, {
            apply( target, thisArg, args ) {
                const e = xhrEdits.length && thisArg.responseType !== 'json' ? editedText( thisArg ) : null;
                return e ? e.value : ReflectApply( target, thisArg, args );
            }
        } )
    } );
}

// a request that was not sent: as if an empty answer came
function prevented( xhr, r )
{
    const json = xhr.responseType === 'json';
    const props = { readyState: 4, status: 200, statusText: 'OK', responseURL: r.url,
                    response: json ? null : '', responseText: '' };
    for ( const k of ObjectKeys( props ) )
        try {
            ObjectDefineProperty( xhr, k, { value: props[k], configurable: true } );
        } catch ( e ) {
        }
    NativeSetTimeout( () => {
        for ( const type of [ 'readystatechange', 'load', 'loadend' ] )
            try {
                xhr.dispatchEvent( new W.Event( type ) );
            } catch ( e ) {
            }
    }, 1 );
}

function jsonEditor( prunePaths, needlePaths )
{
    const prune = makePruner( prunePaths, needlePaths );
    if ( !prune )
        return null;
    return text => {
        let obj;
        try {
            obj = JSONParse( text );
        } catch ( e ) {
            return null;
        }
        return prune( obj ) ? JSONStringify( obj ) : null;
    };
}

function textReplacer( pattern, replacement )
{
    if ( pattern === '' )
        return null;
    if ( pattern === '*' )
        return () => replacement;
    const re = toRegExp( pattern, 'gms' );
    if ( !re )
        return null;
    return text => {
        const out = replace( text, re, replacement );
        return out === text ? null : out;
    };
}

function addEdit( list, props, edit )
{
    const match = props === '' ? () => true : propsMatcher( props );
    if ( !match || !edit )
        return false;
    list.push( { match, edit } );
    return true;
}

function jsonPruneFetchResponse( args )
{
    const [ prunePaths = '', needlePaths = '' ] = args;
    const extra = extraArgs( args, 2 );
    if ( addEdit( fetchEdits, extra.propsToMatch || '', jsonEditor( prunePaths, needlePaths ) ) )
        hookFetch();
}

function jsonPruneXHRResponse( args )
{
    const [ prunePaths = '', needlePaths = '' ] = args;
    const extra = extraArgs( args, 2 );
    if ( addEdit( xhrEdits, extra.propsToMatch || '', jsonEditor( prunePaths, needlePaths ) ) )
        hookXHR();
}

function replaceFetchResponse( args )
{
    const [ pattern = '', replacement = '', props = '' ] = args;
    if ( addEdit( fetchEdits, props, textReplacer( pattern, replacement ) ) )
        hookFetch();
}

function replaceXHRResponse( args )
{
    const [ pattern = '', replacement = '', props = '' ] = args;
    if ( addEdit( xhrEdits, props, textReplacer( pattern, replacement ) ) )
        hookXHR();
}

function preventFetch( args )
{
    const [ props = '', body = '' ] = args;
    if ( props === '' )
        return;
    const match = propsMatcher( props );
    if ( !match )
        return;
    const bodies = { '': '', 'emptyStr': '', 'emptyObj': '{}', 'emptyArr': '[]', 'true': 'true', 'false': 'false' };
    fetchPrevents.push( { match, body: Object.prototype.hasOwnProperty.call( bodies, body ) ? bodies[body] : '' } );
    hookFetch();
}

function preventXHR( args )
{
    const [ props = '' ] = args;
    if ( props === '' )
        return;
    const match = propsMatcher( props );
    if ( !match )
        return;
    xhrPrevents.push( { match } );
    hookXHR();
}

// ---------------------------------------------------------------------
// the document: attributes, classes, inline scripts

function whenReady( behavior, fn )
{
    const tokens = behavior.split( /\s+/ );
    const run = () => {
        try {
            fn();
        } catch ( e ) {
        }
    };
    if ( tokens.includes( 'asap' ) )
        run();
    const start = () => {
        run();
        if ( !tokens.includes( 'stay' ) )
            return;
        let pending = false;
        new NativeMutationObserver( () => {
            if ( pending )
                return;
            pending = true;
            NativeSetTimeout( () => {
                pending = false;
                run();
            }, 0 );
        } ).observe( document, { childList: true, subtree: true, attributes: true } );
    };
    if ( tokens.includes( 'complete' ) ) {
        if ( document.readyState === 'complete' )
            start();
        else
            W.addEventListener( 'load', start, { once: true } );
    } else if ( document.readyState !== 'loading' ) {
        start();
    } else {
        document.addEventListener( 'DOMContentLoaded', start, { once: true } );
    }
}

function validSelector( selector )
{
    try {
        document.createDocumentFragment().querySelector( selector );
        return true;
    } catch ( e ) {
        return false;
    }
}

function removeAttr( args )
{
    const [ attrText = '', selectorArg = '', behavior = '' ] = args;
    const attrs = attrText.split( /\s*\|\s*/ ).filter( a => /^[^\s"'<>\/=]+$/.test( a ) );
    if ( attrs.length === 0 )
        return;
    const selector = selectorArg || attrs.map( a => '[' + CSS.escape( a ) + ']' ).join( ',' );
    if ( !validSelector( selector ) )
        return;
    whenReady( behavior, () => {
        for ( const e of document.querySelectorAll( selector ) )
            for ( const a of attrs )
                e.removeAttribute( a );
    } );
}

function removeClass( args )
{
    const [ classText = '', selectorArg = '', behavior = '' ] = args;
    const classes = classText.split( /\s*\|\s*/ ).filter( c => /^\S+$/.test( c ) );
    if ( classes.length === 0 )
        return;
    const selector = selectorArg || classes.map( c => '.' + CSS.escape( c ) ).join( ',' );
    if ( !validSelector( selector ) )
        return;
    whenReady( behavior, () => {
        for ( const e of document.querySelectorAll( selector ) )
            e.classList.remove( ...classes );
    } );
}

// Nodes as the parser adds them, before an inline script runs (the
// parser runs the observers first).
const nodeHooks = [];

function hookNodes()
{
    if ( nodeHooks.observer )
        return;
    const seen = node => {
        for ( const h of nodeHooks ) {
            if ( h.done || node.nodeName.toLowerCase() !== h.name )
                continue;
            try {
                if ( h.fn( node ) === false )
                    return;
            } catch ( e ) {
            }
        }
    };
    nodeHooks.observer = new NativeMutationObserver( records => {
        for ( const r of records ) {
            // text added to a script that was already seen
            if ( r.target && r.target.nodeType === 1 && r.target !== document.documentElement && r.target.nodeName === 'SCRIPT' )
                seen( r.target );
            for ( const n of r.addedNodes )
                seen( n );
        }
    } );
    nodeHooks.observer.observe( document, { childList: true, subtree: true } );
}

function removeNodeText( args )
{
    const [ nodeName = '', needle = '' ] = args;
    if ( nodeName === '' || needle === '' )
        return;
    const re = toRegExp( needle );
    if ( !re )
        return;
    nodeHooks.push( { name: nodeName.toLowerCase(), fn: node => {
        if ( !test( re, node.textContent ) )
            return true;
        if ( node.nodeName === 'SCRIPT' )
            node.textContent = '';
        node.remove();
        return false;
    } } );
    hookNodes();
}

function replaceNodeText( args )
{
    const [ nodeName = '', pattern = '', replacement = '' ] = args;
    const extra = extraArgs( args, 3 );
    if ( nodeName === '' || pattern === '' )
        return;
    const re = toRegExp( pattern, 'gms' );
    const condition = extra.condition ? toRegExp( extra.condition ) : /^/;
    if ( !re || !condition )
        return;
    let count = parseInt( extra.sedCount || '0', 10 ) || 0;
    const hook = { name: nodeName.toLowerCase(), fn: node => {
        const text = node.textContent;
        if ( !test( condition, text ) )
            return true;
        const out = replace( text, re, replacement );
        if ( out === text )
            return true;
        node.textContent = out;
        if ( count > 0 && --count === 0 )
            hook.done = true;
        return true;
    } };
    nodeHooks.push( hook );
    hookNodes();
}

// A frame added to the page gets our patched function (pages take a
// clean one from a new frame).
function preventDOMBypass( args )
{
    const [ methodPath = '', targetProp = '' ] = args;
    if ( methodPath === '' || targetProp === '' )
        return;
    const segs = methodPath.split( '.' );
    const name = segs.pop();
    let owner = W;
    for ( const s of segs ) {
        owner = isObject( owner ) ? owner[s] : undefined;
    }
    if ( !isObject( owner ) || typeof owner[name] !== 'function' )
        return;
    const propSegs = targetProp.split( '.' );
    const patch = frame => {
        let w;
        try {
            w = frame.contentWindow;
        } catch ( e ) {
            return;
        }
        if ( !w )
            return;
        try {
            let src = W, dst = w;
            for ( let i = 0; i < propSegs.length - 1; ++i ) {
                src = src[propSegs[i]];
                dst = dst[propSegs[i]];
            }
            const last = propSegs[propSegs.length - 1];
            dst[last] = src[last];
        } catch ( e ) {
        }
    };
    owner[name] = new NativeProxy( owner[name], {
        apply( target, thisArg, a ) {
            const result = ReflectApply( target, thisArg, a );
            for ( const n of a ) {
                try {
                    if ( !isObject( n ) || n.nodeType !== 1 )
                        continue;
                    if ( n.nodeName === 'IFRAME' || n.nodeName === 'FRAME' )
                        patch( n );
                    for ( const f of n.querySelectorAll( 'iframe,frame' ) )
                        patch( f );
                } catch ( e ) {
                }
            }
            return result;
        }
    } );
}

// ---------------------------------------------------------------------

const scriptlets = {
    'set-constant': a => setConstant( a, false ),
    'trusted-set-constant': a => setConstant( a, true ),
    'abort-on-property-read': abortOnPropertyRead,
    'abort-on-property-write': abortOnPropertyWrite,
    'abort-current-script': abortCurrentScript,
    'abort-on-stack-trace': abortOnStackTrace,
    'no-setTimeout-if': a => noTimerIf( 'setTimeout', a ),
    'no-setInterval-if': a => noTimerIf( 'setInterval', a ),
    'adjust-setTimeout': a => adjustTimer( 'setTimeout', a ),
    'adjust-setInterval': a => adjustTimer( 'setInterval', a ),
    'noeval-if': noevalIf,
    'prevent-window-open': preventWindowOpen,
    'prevent-addEventListener': preventAddEventListener,
    'remove-attr': removeAttr,
    'remove-class': removeClass,
    'json-prune': jsonPrune,
    'json-prune-fetch-response': jsonPruneFetchResponse,
    'json-prune-xhr-response': jsonPruneXHRResponse,
    'trusted-replace-fetch-response': replaceFetchResponse,
    'trusted-replace-xhr-response': replaceXHRResponse,
    'prevent-fetch': preventFetch,
    'prevent-xhr': preventXHR,
    'remove-node-text': removeNodeText,
    'replace-node-text': replaceNodeText,
    'trusted-replace-node-text': replaceNodeText,
    'trusted-prevent-dom-bypass': preventDOMBypass,
};

function run( calls )
{
    for ( const call of calls ) {
        const fn = scriptlets[call[0]];
        if ( !fn )
            continue;
        try {
            fn( call.slice( 1 ) );
        } catch ( e ) {
        }
    }
}
